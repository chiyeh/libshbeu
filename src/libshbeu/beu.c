/*
 * libshbeu: A library for controlling SH-Mobile BEU
 * Copyright (C) 2010 Renesas Technology Corp.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA  02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>

#include <uiomux/uiomux.h>
#include "shbeu/shbeu.h"
#include "shbeu_regs.h"

#define DEBUG

struct uio_map {
	unsigned long address;
	unsigned long size;
	void *iomem;
};

struct SHBEU {
	UIOMux *uiomux;
	struct uio_map uio_mmio;
};


/* Helper functions for reading registers. */

static unsigned long read_reg(struct uio_map *ump, int reg_nr)
{
	volatile unsigned long *reg = ump->iomem + reg_nr;

#ifdef DEBUG
	fprintf(stderr, "read_reg[0x%X] returned %u\n", reg_nr, *reg);
#endif

	return *reg;
}

static void write_reg(struct uio_map *ump, unsigned long value, int reg_nr)
{
	volatile unsigned long *reg = ump->iomem + reg_nr;

#ifdef DEBUG
	fprintf(stderr, "write_reg[0x%X] = %u\n", reg_nr, *reg);
#endif

	*reg = value;
}

void shbeu_close(SHBEU *pvt)
{
	if (pvt) {
		if (pvt->uiomux)
			uiomux_close(pvt->uiomux);
		free(pvt);
	}
}

SHBEU *shbeu_open(void)
{
	SHBEU *beu;
	int ret;

	beu = calloc(1, sizeof(*beu));
	if (!beu)
		goto err;

	beu->uiomux = uiomux_open();
	if (!beu->uiomux)
		goto err;

	ret = uiomux_get_mmio (beu->uiomux, UIOMUX_SH_BEU,
		&beu->uio_mmio.address,
		&beu->uio_mmio.size,
		&beu->uio_mmio.iomem);
	if (!ret)
		goto err;

	return beu;

err:
	shbeu_close(beu);
	return 0;
}

static int is_ycbcr(int fmt)
{
	if ((fmt == V4L2_PIX_FMT_NV12) || (fmt == V4L2_PIX_FMT_NV16))
		return 1;
	return 0;
}

static int is_rgb(int fmt)
{
	if ((fmt == V4L2_PIX_FMT_RGB565) || (fmt == V4L2_PIX_FMT_RGB32))
		return 1;
	return 0;
}

static int
setup_src_surface(struct uio_map *ump, int index, beu_surface_t *surface)
{
	const int offsets[] = {SRC1_BASE, SRC2_BASE, SRC3_BASE};
	int offset = offsets[index];
	unsigned long tmp;
	unsigned long fmt_reg;

	if (!surface)
		return 0;

#ifdef DEBUG
	fprintf(stderr, "src%d: fmt=%d: width=%lu, height=%lu pitch=%lu\n",
		index+1, surface->format, surface->width, surface->height, surface->pitch);
#endif

	if (!surface->py)
		return -1;

	if ((surface->width % 4) || (surface->pitch % 4) || (surface->height % 4))
		return -1;

	if ((surface->width > 4092) || (surface->pitch > 4092) || (surface->height > 4092))
		return -1;

	tmp = (surface->height << 16) | surface->width;
	write_reg(ump, tmp, BSSZR + offset);
	write_reg(ump, surface->pitch, BSMWR + offset);
	write_reg(ump, surface->py, BSAYR + offset);
	write_reg(ump, surface->pc, BSACR + offset);
	write_reg(ump, surface->pa, BSAAR + offset);

	/* Surface format */
	if ((surface->format == V4L2_PIX_FMT_NV12) && !surface->pa)
		fmt_reg = (0x2 << 8);
	else if ((surface->format == V4L2_PIX_FMT_NV12) && surface->pa)
		fmt_reg = (0x5 << 8);
	else if ((surface->format == V4L2_PIX_FMT_NV16) && !surface->pa)
		fmt_reg = (0x1 << 8);
	else if ((surface->format == V4L2_PIX_FMT_NV16) && surface->pa)
		fmt_reg = (0x4 << 8);
	else if ((surface->format == V4L2_PIX_FMT_RGB565) && !surface->pa)
		fmt_reg = 0x3;
	else if ((surface->format == V4L2_PIX_FMT_RGB32) && !surface->pa)
		fmt_reg = 0x0;
	else if ((surface->format == V4L2_PIX_FMT_RGB32) && surface->pa)
		fmt_reg = 0xC;
	else
		return -1;
	write_reg(ump, fmt_reg, BSIFR + offset);

	/* Position of overlay */
	tmp = (surface->x << 16) | surface->y;
	write_reg(ump, tmp, BLOCR1 + index*4);

	/* byte/word swapping */
	tmp = read_reg(ump, BSWPR);
	tmp |= (1 << 31);
	if (surface->format == V4L2_PIX_FMT_RGB565)
		tmp |= ((0x6 << (index+1)*8));
	else
		tmp |= ((0x7 << (index+1)*8));
	write_reg(ump, tmp, BSWPR);

	/* Set alpha value for entire plane, if no alpha data */
	tmp = read_reg(ump, BBLCR0);
	if (!surface->pa)
		tmp |= ((surface->alpha & 0xFF) << index*8);
	write_reg(ump, 0xFFFFFF, BBLCR0);

	return 0;
}

static int
setup_dst_surface(struct uio_map *ump, beu_surface_t *surface)
{
	unsigned long tmp;
	unsigned long fmt_reg;

	if (!surface)
		return 0;

#ifdef DEBUG
	fprintf(stderr, "dest: fmt=%d: width=%lu, height=%lu pitch=%lu\n",
		surface->format, surface->width, surface->height, surface->pitch);
#endif

	if (surface->pa || surface->x || surface->y)
		return -1;

	if ((surface->width % 4) || (surface->pitch % 4) || (surface->height % 4))
		return -1;

	if ((surface->width > 4092) || (surface->pitch > 4092) || (surface->height > 4092))
		return -1;

	// TODO The dest size is not needed? the pitch limits the overlay to the dest memory
	// but what about the height?
	write_reg(ump, surface->pitch, BDMWR);
	write_reg(ump, surface->py, BDAYR);
	write_reg(ump, surface->pc, BDACR);
	write_reg(ump, 0, BAFXR);

	/* Surface format */
	if (surface->format == V4L2_PIX_FMT_NV12)
		fmt_reg = (0x2 << 8);
	else if (surface->format == V4L2_PIX_FMT_NV16)
		fmt_reg = (0x1 << 8);
	else if (surface->format == V4L2_PIX_FMT_RGB565)
		fmt_reg = 0x6;
	else if (surface->format == V4L2_PIX_FMT_RGB32)
		fmt_reg = 0xB;
	else
		return -1;
	write_reg(ump, fmt_reg, BPKFR);

	/* byte/word swapping */
	tmp = read_reg(ump, BSWPR);
	if (surface->format == V4L2_PIX_FMT_RGB565)
		tmp |= 0x60;
	else
		tmp |= 0x70;
	write_reg(ump, tmp, BSWPR);

	return 0;
}

int
shbeu_start_blend(
	SHBEU *pvt,
	beu_surface_t *src1,
	beu_surface_t *src2,
	beu_surface_t *src3,
	beu_surface_t *dest)
{
	struct uio_map *ump = &pvt->uio_mmio;
	unsigned long start_reg = BESTR_BEIVK;
	unsigned long control_reg;

#ifdef DEBUG
	fprintf(stderr, "%s IN\n", __func__);
#endif

	/* Check we have been passed at least 2 inputs and an output */
	if (!pvt || !src1 || !src2 || !dest)
		return -1;

	/* Check src2 and src3 formats are the same */
	if (src3) {
		if (src2->format != src3->format)
			return -1;
	}


	uiomux_lock (pvt->uiomux, UIOMUX_SH_BEU);

	/* reset */
	write_reg(ump, 1, BBRSTR);

	write_reg(ump, 0, BRCNTR);
	write_reg(ump, 0, BRCHR);

	/* Set surface 1 to back, surface 2 to middle & surface 2 to front */
	write_reg(ump, 0, BBLCR0);

	if (setup_src_surface(ump, 0, src1) < 0)
		goto err;
	if (setup_src_surface(ump, 1, src2) < 0)
		goto err;
	if (setup_src_surface(ump, 2, src3) < 0)
		goto err;
	if (setup_dst_surface(ump, dest) < 0)
		goto err;

	/* Are the input colourspaces different? */
	if ((is_ycbcr(src1->format) && is_ycbcr(src2->format))
	    || (is_rgb(src1->format) && is_rgb(src2->format)))
	{
		unsigned long bsifr = read_reg(ump, BSIFR);
		bsifr |= BSIFR1_IN1TE;
		write_reg(ump, bsifr, BSIFR);
	}

	/* Is the output colourspace different to input 2? */
	if ((is_ycbcr(dest->format) && is_ycbcr(src2->format))
	    || (is_rgb(dest->format) && is_rgb(src2->format)))
	{
		unsigned long bpkfr = read_reg(ump, BPKFR);
		bpkfr |= BPKFR_RY;
		if (is_rgb(src2->format)) bpkfr |= BPKFR_TE;
		write_reg(ump, bpkfr, BPKFR);
	}

	/* Set surface 1 as the parent; output to memory */
	write_reg(ump, BBLCR1_OUTPUT_MEM, BBLCR1);

	write_reg(ump, 0, BPROCR);
	write_reg(ump, 0, BMWCR0);
	write_reg(ump, 0, BPCCR0);

	/* enable interrupt */
	write_reg(ump, 1, BEIER);

	/* start operation */
	if (src1) start_reg |= BESTR_CHON1;
	if (src2) start_reg |= BESTR_CHON2;
	if (src3) start_reg |= BESTR_CHON3;
	write_reg(ump, start_reg, BESTR);


#ifdef DEBUG
	fprintf(stderr, "%s OUT\n", __func__);
#endif

	return 0;

err:
	uiomux_unlock(pvt->uiomux, UIOMUX_SH_BEU);
	return -1;
}

void
shbeu_wait(SHBEU *pvt)
{
	uiomux_sleep(pvt->uiomux, UIOMUX_SH_BEU);

	write_reg(&pvt->uio_mmio, 0x100, BEVTR);   /* ack int, write 0 to bit 0 */

	/* Wait for BEU to stop */
	while (read_reg(&pvt->uio_mmio, BSTAR) & 1)
		;

	uiomux_unlock(pvt->uiomux, UIOMUX_SH_BEU);
}


int
shbeu_blend(
	SHBEU *pvt,
	beu_surface_t *src1,
	beu_surface_t *src2,
	beu_surface_t *src3,
	beu_surface_t *dest)
{
	int ret = 0;

	ret = shbeu_start_blend(pvt, src1, src2, src3, dest);

	if (ret == 0)
		shbeu_wait(pvt);

	return ret;
}

