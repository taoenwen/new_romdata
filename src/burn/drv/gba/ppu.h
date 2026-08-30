#pragma once

#ifndef GBA_PPU_H
#define GBA_PPU_H

#include "gba.h"

#define GBA_LCD_HBLANK_END		(295)
#define GBA_LCD_HBLANK_START	(GBA_LCD_W)
#define GBA_LCD_VBLANK_START	(GBA_LCD_H*1232)

// Max cycles to skip from the current beam position
static inline INT32 gba_ppu_compute_max_fast_forward(gba_t* gba, bool render)
{
	INT32 scanline_clock = (gba->ppu.scan_clock) % 1232;
	// inside hblank: fast-forward to its end
	if (scanline_clock >= GBA_LCD_HBLANK_START * 4 && scanline_clock <= GBA_LCD_HBLANK_END * 4)
		return GBA_LCD_HBLANK_END   * 4 - scanline_clock - 1;
	// inside hrender: fast-forward to hblank if not visible
	bool not_visible = !render || gba->ppu.scan_clock > GBA_LCD_VBLANK_START;
	if (not_visible && (scanline_clock >= 1 && scanline_clock <= GBA_LCD_HBLANK_START * 4))
		return GBA_LCD_HBLANK_START * 4 - scanline_clock - 1;
	return 3 - ((gba->ppu.scan_clock) % 4);
}

// Renders the OBJ layer and builds the window mask for one scanline
static inline void gba_ppu_render_objs(gba_t* gba, INT32 sprite_lcd_y)
{
	UINT16 dispcnt         = gba_io_read16(gba, GBA_DISPCNT);
	INT32  bg_mode         = SB_BFE(dispcnt, 0, 3);
	INT32  obj_vram_map_2d = !SB_BFE(dispcnt, 6, 1);

	UINT16 mos_reg = gba_io_read16(gba, GBA_MOSAIC);
	INT32  mos_y   = SB_BFE(mos_reg, 12, 4) + 1;
	// SkyEmu issue 316: mosaic counter fix
	if (++gba->ppu.mosaic_y_counter >= mos_y || sprite_lcd_y == 0) {
		gba->ppu.mosaic_y_counter = 0;
	}
	UINT8 default_window_control = 0x3f;	//bitfield [0-3:bg0-bg3 enable 4:obj enable, 5: special effect enable]
	bool winout_enable = SB_BFE(dispcnt, 13, 3) != 0;
	UINT16 WINOUT      = gba_io_read16(gba, GBA_WINOUT);
	if (winout_enable)
		default_window_control = SB_BFE(WINOUT, 0, 8);

	for (INT32 x = 0; x < 240; ++x) {
		gba->window[x] = default_window_control;
	}
	UINT8 obj_window_control = default_window_control;
	bool  obj_window_enable  = SB_BFE(dispcnt, 15, 1);
	if (obj_window_enable)
		obj_window_control   = SB_BFE(WINOUT, 8, 6);
	bool  display_obj        = SB_BFE(dispcnt, 12, 1);
	if (display_obj) {
		INT32 sprite_cycles  = SB_BFE(dispcnt, 5, 1) ? 954 : 1210;
		for (INT32 o = 0; o < 128; ++o) {
			UINT16 attr0 = *(UINT16*)(gba->mem.oam + o * 8 + 0);
			//Attr0
			UINT8 y_coord     = SB_BFE(attr0, 0, 8);
			bool  rot_scale   = SB_BFE(attr0, 8, 1);
			bool  double_size = SB_BFE(attr0, 9, 1) && rot_scale;
			bool  obj_disable = SB_BFE(attr0, 9, 1) && !rot_scale;
			if (obj_disable)
				continue;

			INT32  obj_mode           = SB_BFE(attr0, 10, 2);	//(0=Normal, 1=Semi-transparent, 2=OBJ Window, 3=Prohibited)
			bool   mosaic             = SB_BFE(attr0, 12, 1);
			bool   colors_or_palettes = SB_BFE(attr0, 13, 1);
			INT32  obj_shape          = SB_BFE(attr0, 14, 2);	//(0=Square,1=Horizontal,2=Vertical,3=Prohibited)
			UINT16 attr1 = *(UINT16*)(gba->mem.oam + o * 8 + 2);

			INT32 rotscale_param = SB_BFE(attr1,  9, 5);
			bool  h_flip         = SB_BFE(attr1, 12, 1) && !rot_scale;
			bool  v_flip         = SB_BFE(attr1, 13, 1) && !rot_scale;
			INT32 obj_size       = SB_BFE(attr1, 14, 2);
			// Size  Square   Horizontal  Vertical
			// 0     8x8      16x8        8x16
			// 1     16x16    32x8        8x32
			// 2     32x32    32x16       16x32
			// 3     64x64    64x32       32x64
			const INT32 xsize_lookup[16] = {
				 8,16, 8, 0,
				16,32, 8, 0,
				32,32,16, 0,
				64,64,32, 0
			};
			const INT32 ysize_lookup[16] = {
				 8, 8,16, 0,
				16, 8,32, 0,
				32,16,32, 0,
				64,32,64, 0
			};

			INT32 y_size = ysize_lookup[obj_size * 4 + obj_shape];

			if (((sprite_lcd_y - y_coord) & 0xff) < y_size * (double_size ? 2 : 1)) {
				INT16 x_coord = SB_BFE(attr1, 0, 9);
				if (SB_BFE(x_coord, 8, 1))
					x_coord |= 0xfe00;

				INT32 x_size  = xsize_lookup[obj_size * 4 + obj_shape];
				if (rot_scale)
					sprite_cycles -= 10 + (x_size << double_size) * 2;
				else
					sprite_cycles -= x_size;
				if (sprite_cycles <= 0)
					break;
				INT32 x_start = x_coord >= 0 ? x_coord : 0;
				INT32 x_end   = x_coord + x_size * (double_size ? 2 : 1);
				if (x_end >= 240)x_end = 240;
				//Attr2
				//Skip objects disabled by window
				UINT16 attr2 = *(UINT16*)(gba->mem.oam + o * 8 + 4);
				INT32  tile_base = SB_BFE(attr2,  0, 10);
				// Always place sprites as the highest priority
				INT32  priority  = SB_BFE(attr2, 10,  2);
				INT32  palette   = SB_BFE(attr2, 12,  4);
				for (INT32 x = x_start; x < x_end; ++x) {
					INT32 sx = (x - x_coord);
					INT32 sy = (sprite_lcd_y - y_coord) & 0xff;
					if (mosaic) {
						UINT16 mos_reg2 = gba_io_read16(gba, GBA_MOSAIC);
						INT32  mos_x = SB_BFE(mos_reg2,  8, 4) + 1;
						sx = ((x / mos_x) * mos_x - x_coord);
						if (sx < 0)
							sx = 0;
						sy = (sprite_lcd_y - y_coord) & 0xff;
						sy -= gba->ppu.mosaic_y_counter;
						if (sy < 0) {
							sy = 0;
						}
					}
					if (rot_scale) {
						UINT32 param_base = rotscale_param * 0x20;
						INT32 a = *(INT16*)(gba->mem.oam + param_base + 0x6);
						INT32 b = *(INT16*)(gba->mem.oam + param_base + 0xe);
						INT32 c = *(INT16*)(gba->mem.oam + param_base + 0x16);
						INT32 d = *(INT16*)(gba->mem.oam + param_base + 0x1e);

						INT64 x1 = sx << 8;
						INT64 y1 = sy << 8;
						INT64 objref_x = (x_size << (double_size ? 8 : 7));
						INT64 objref_y = (y_size << (double_size ? 8 : 7));

						INT64 x2 = a * (x1 - objref_x) + b * (y1 - objref_y) + (x_size << 15);
						INT64 y2 = c * (x1 - objref_x) + d * (y1 - objref_y) + (y_size << 15);

						sx = (x2 >> 16);
						sy = (y2 >> 16);
						if (sx >= x_size || sy >= y_size || sx < 0 || sy < 0)
							continue;
					} else {
						if (h_flip)
							sx = x_size - sx - 1;
						if (v_flip)
							sy = y_size - sy - 1;
					}
					INT32 tx = sx % 8;
					INT32 ty = sy % 8;

					INT32 tile_step = colors_or_palettes ? 2 : 1;
					INT32 tile;
					if (obj_vram_map_2d) {
						INT32 base = colors_or_palettes ? tile_base & ~1 : tile_base;
						tile  = (base + (sx / 8) * tile_step) & 0x1f;
						tile |= (base + (sy / 8) * 32       ) & 0x3e0;
					} else {
						tile  = (tile_base + (sx / 8) * tile_step + (sy / 8) * (x_size / 8) * tile_step) & 0x3ff;
					}
					//Tiles >511 are not rendered in bg_mode3-5 since that memory is used to store the bitmap graphics.
					if (tile < 512 && bg_mode >= 3 && bg_mode <= 5)
						continue;
					UINT8 palette_id;
					INT32 obj_tile_base = GBA_OBJ_TILES0_2;
					bool transparent = false;
					if (colors_or_palettes == false) {
						INT32 offset = (tile * 32 + tx / 2 + ty * 4) & 0x7fff;
						palette_id   = gba->mem.vram[obj_tile_base + offset];
						palette_id   = (palette_id >> ((tx & 1) * 4)) & 0xf;
						transparent  =  palette_id == 0;
						palette_id  +=  palette * 16;
					} else {
						INT32 offset = (tile * 32 + tx + ty * 8) & 0x7fff;
						palette_id   = gba->mem.vram[obj_tile_base + offset];
						transparent  = palette_id == 0;
					}

					UINT32 col = *(UINT16*)(gba->mem.palette + GBA_OBJ_PALETTE + palette_id * 2);
					//Handle window objects(not displayed but control the windowing of other things)
					if (obj_mode == 2 && !transparent) {
						gba->window[x] = obj_window_control;
					} else if (obj_mode != 3) {
						INT32 type = 4;
						col = col | (type << 17) | ((5 - priority) << 28) | ((0x7) << 25);
						if (obj_mode == 1)
							col |= 1 << 16;
						if ((col >> 17) > (gba->first_target_buffer[x] >> 17)) {
							if (transparent) {
								//Update priority for transparent pixels (needed for golden sun)
								if (SB_BFE(gba->first_target_buffer[x], 17, 3) != 5)
									gba->first_target_buffer[x] = (gba->first_target_buffer[x] & (0x0fffffff)) | (col & 0xf0000000);
							} else gba->first_target_buffer[x] = col;
						}
					}
				}
			}
		}
	}
	INT32 enabled_windows = SB_BFE(dispcnt, 13, 3);		// [0: win0, 1:win1, 2: objwin]
	if (enabled_windows) {
		for (INT32 win = 1; win >= 0; --win) {
			bool win_enable = SB_BFE(dispcnt, 13 + win, 1);
			if (!win_enable)
				continue;
			UINT16 WINH = gba_io_read16(gba, GBA_WIN0H + 2 * win);
			UINT16 WINV = gba_io_read16(gba, GBA_WIN0V + 2 * win);
			INT32  win_xmin = SB_BFE(WINH, 8, 8);
			INT32  win_xmax = SB_BFE(WINH, 0, 8);
			INT32  win_ymin = SB_BFE(WINV, 8, 8);
			INT32  win_ymax = SB_BFE(WINV, 0, 8);
			// Garbage values of X2>240 or X1>X2 are interpreted as X2=240.
			// Garbage values of Y2>160 or Y1>Y2 are interpreted as Y2=160.
			if (win_xmin > win_xmax)
				win_xmax = 240;
			if (win_ymin > win_ymax)
				win_ymax = 161;
			if (win_xmax > 240)
				win_xmax = 240;
			if (sprite_lcd_y < win_ymin || sprite_lcd_y >= win_ymax)
				continue;
			UINT16 winin = gba_io_read16(gba, GBA_WININ);
			UINT8  win_value = SB_BFE(winin, win * 8, 6);
			for (INT32 x = win_xmin; x < win_xmax; ++x)
				gba->window[x] = win_value;
		}
		INT32  backdrop_type = 5;
		UINT32 backdrop_col  = (*(UINT16*)(gba->mem.palette + GBA_BG_PALETTE + 0 * 2)) | (backdrop_type << 17);
		for (INT32 x = 0; x < 240; ++x) {
			UINT8 window_control = gba->window[x];
			if (SB_BFE(window_control, 4, 1) == 0)
				gba->first_target_buffer[x] = backdrop_col;
		}
	}
}

// Samples and composites one pixel; live register reads give snapshot semantics
static inline void gba_ppu_render_pixel(gba_t* gba, INT32 lcd_x, INT32 lcd_y)
{
	UINT16 dispcnt = gba_io_read16(gba, GBA_DISPCNT);
	INT32  bg_mode      = SB_BFE(dispcnt, 0, 3);
//	INT32  obj_vram_map_2d = !SB_BFE(dispcnt, 6, 1);
	INT32  forced_blank = SB_BFE(dispcnt, 7, 1);
	UINT8  window_control = gba->window[lcd_x];
	if (bg_mode == 6 || bg_mode == 7) {
		//Palette 0 is taken as the background
	} else if (bg_mode <= 5) {
		for (INT32 bg = 3; bg >= 0; --bg) {
			UINT32 col = 0;
			if ((bg < 2 && bg_mode == 2) || (bg == 3 && bg_mode == 1) || (bg != 2 && bg_mode >= 3))
				continue;
			bool bg_en = SB_BFE(dispcnt, 8 + bg, 1);
			if (!bg_en || SB_BFE(window_control, bg, 1) == 0)
				continue;

			bool   rot_scale = bg_mode >= 1 && bg >= 2;
			UINT16 bgcnt = gba_io_read16(gba, GBA_BG0CNT + bg * 2);
			INT32  priority         = SB_BFE(bgcnt,  0, 2);
			INT32  character_base   = SB_BFE(bgcnt,  2, 2);
			bool   mosaic           = SB_BFE(bgcnt,  6, 1);
			bool   colors           = SB_BFE(bgcnt,  7, 1);
			INT32  screen_base      = SB_BFE(bgcnt,  8, 5);
			bool   display_overflow = SB_BFE(bgcnt, 13, 1);
			INT32  screen_size      = SB_BFE(bgcnt, 14, 2);

			INT32 screen_size_x = (screen_size  & 1) ? 512 : 256;
			INT32 screen_size_y = (screen_size >= 2) ? 512 : 256;

			INT32 bg_x = 0;
			INT32 bg_y = 0;

			if (rot_scale) {
				screen_size_x = screen_size_y = (16 * 8) << screen_size;
				if (bg_mode == 3 || bg_mode == 4) {
					screen_size_x = 240;
					screen_size_y = 160;
				} else if (bg_mode == 5) {
					screen_size_x = 160;
					screen_size_y = 128;
				}
				colors = true;

				INT32 bgx = gba->ppu.aff[bg - 2].render_bgx;
				INT32 bgy = gba->ppu.aff[bg - 2].render_bgy;

				INT32 a = (INT16)gba_io_read16(gba, GBA_BG2PA + (bg - 2) * 0x10);
				INT32 c = (INT16)gba_io_read16(gba, GBA_BG2PC + (bg - 2) * 0x10);

				// Shift lcd_coords into fixed point
				INT64 x2 = a * lcd_x + (((INT64)bgx));
				INT64 y2 = c * lcd_x + (((INT64)bgy));
				if (mosaic) {
					INT16 mos_reg = gba_io_read16(gba, GBA_MOSAIC);
					INT32 mos_x   = SB_BFE(mos_reg, 0, 4) + 1;
					x2 = a * ((lcd_x / mos_x) * mos_x) + (((INT64)bgx));
					y2 = c * ((lcd_x / mos_x) * mos_x) + (((INT64)bgy));
				}


				bg_x = (x2 >> 8);
				bg_y = (y2 >> 8);

				if (display_overflow == 0) {
					if (bg_x < 0 || bg_x >= screen_size_x || bg_y < 0 || bg_y >= screen_size_y)
						continue;
				} else {
					bg_x %= screen_size_x;
					bg_y %= screen_size_y;
				}
			} else {
				INT16 hoff = gba_io_read16(gba, GBA_BG0HOFS + bg * 4);
				INT16 voff = gba_io_read16(gba, GBA_BG0VOFS + bg * 4);
				hoff = (hoff << 7) >> 7;
				voff = (voff << 7) >> 7;
				bg_x = (hoff + lcd_x);
				bg_y = (voff + lcd_y);
				if (mosaic) {
					UINT16 mos_reg = gba_io_read16(gba, GBA_MOSAIC);
					INT32  mos_x = SB_BFE(mos_reg, 0, 4) + 1;
					INT32  mos_y = SB_BFE(mos_reg, 4, 4) + 1;
					bg_x = hoff + (lcd_x / mos_x) * mos_x;
					bg_y = voff + (lcd_y / mos_y) * mos_y;
				}
			}
			if (bg_mode == 3) {
				INT32 p    = bg_x + bg_y * 240;
				INT32 addr = p * 2;
				col = *(UINT16*)(gba->mem.vram + addr);
			} else if (bg_mode == 4) {
				INT32 p          = bg_x + bg_y * 240;
				INT32 frame_sel  = SB_BFE(dispcnt, 4, 1);
				INT32 addr       = p * 1 + 0xA000 * frame_sel;
				UINT8 palette_id = gba->mem.vram[addr];
				if (palette_id == 0)
					continue;
				col = *(UINT16*)(gba->mem.palette + GBA_BG_PALETTE + palette_id * 2);
			} else if (bg_mode == 5) {
				INT32 p         = bg_x + bg_y * 160;
				INT32 frame_sel = SB_BFE(dispcnt, 4, 1);
				INT32 addr      = p * 2 + 0xA000 * frame_sel;
				col = *(UINT16*)(gba->mem.vram + addr);
			} else {
				bg_x = bg_x & (screen_size_x - 1);
				bg_y = bg_y & (screen_size_y - 1);
				INT32 bg_tile_x = bg_x / 8;
				INT32 bg_tile_y = bg_y / 8;

				INT32 tile_off  = bg_tile_y * (screen_size_x / 8) + bg_tile_x;

				INT32 screen_base_addr    = screen_base * 2048;
				INT32 character_base_addr = character_base * 16 * 1024;

				UINT16 tile_data = 0;

				INT32 px = bg_x % 8;
				INT32 py = bg_y % 8;

				if (rot_scale)tile_data = gba->mem.vram[screen_base_addr + tile_off];
				else {
					INT32 tile_off2 = (bg_tile_y % 32) * 32 + (bg_tile_x % 32);
					if (bg_tile_x >= 32)
						tile_off2 += 32 * 32;
					if (bg_tile_y >= 32)
						tile_off2 += 32 * 32 * (screen_size == 3 ? 2 : 1);
					tile_data = *(UINT16*)(gba->mem.vram + screen_base_addr + tile_off2 * 2);

					INT32 h_flip = SB_BFE(tile_data, 10, 1);
					INT32 v_flip = SB_BFE(tile_data, 11, 1);
					if (h_flip)
						px = 7 - px;
					if (v_flip)
						py = 7 - py;
				}
				INT32 tile_id = SB_BFE(tile_data,  0, 10);
				INT32 palette = SB_BFE(tile_data, 12,  4);

				UINT8 tile_d = tile_id;
				if (colors == false) {
					INT32 addr = character_base_addr + tile_id * 8 * 4 + px / 2 + py * 4;
					tile_d = gba->mem.vram[addr];
					tile_d = (tile_d >> ((px & 1) * 4)) & 0xf;
					//There is an undocumented GBA quirk where tiles over 64KB are not loaded
					if (tile_d == 0 || SB_UNLIKELY(addr >= 0x10000))
						continue;
					tile_d += palette * 16;
				} else {
					//There is an undocumented GBA quirk where tiles over 64KB are not loaded
					INT32 addr = character_base_addr + tile_id * 8 * 8 + px + py * 8;
					tile_d = gba->mem.vram[addr];
					if (tile_d == 0 || SB_UNLIKELY(addr >= 0x10000))
						continue;
				}
				UINT8 palette_id = tile_d;
				col = *(UINT16*)(gba->mem.palette + GBA_BG_PALETTE + palette_id * 2);
			}
			col |= (bg << 17) | ((5 - priority) << 28) | ((4 - bg) << 25);
			if (col > gba->first_target_buffer[lcd_x]) {
				UINT32 t = gba->first_target_buffer[lcd_x];
				gba->first_target_buffer[lcd_x] = col;
				col = t;
			}
			if (col > gba->second_target_buffer[lcd_x])
				gba->second_target_buffer[lcd_x] = col;
		}
	}
	UINT32 col  = gba->first_target_buffer[lcd_x];
	INT32  r    = SB_BFE(col,  0, 5);
	INT32  g    = SB_BFE(col,  5, 5);
	INT32  b    = SB_BFE(col, 10, 5);
	UINT32 type = SB_BFE(col, 17, 3);

	bool   effect_enable = SB_BFE(window_control, 5, 1);
	UINT16 bldcnt        = gba_io_read16(gba, GBA_BLDCNT);
	INT32  mode          = SB_BFE(bldcnt        , 6, 2);

	//Semitransparent objects are always selected for blending
	if (SB_BFE(col, 16, 1)) {
		UINT32 col2  = gba->second_target_buffer[lcd_x];
		UINT32 type2 = SB_BFE(col2,   17,         3);
		bool   blend = SB_BFE(bldcnt,  8 + type2, 1);
		if (blend) {
			mode          = 1;
			effect_enable = true;
		} else effect_enable &= SB_BFE(bldcnt, type, 1);
	} else effect_enable &= SB_BFE(bldcnt, type, 1);
	if (effect_enable) {
		UINT16 bldy = gba_io_read16(gba, GBA_BLDY);
		float  evy  = SB_BFE(bldy, 0, 5) / 16.;
		if (evy > 1.0)
			evy = 1;
		switch (mode) {
			case 0:
				break;	//None
			case 1: {
				UINT32 col2  = gba->second_target_buffer[lcd_x];
				UINT32 type2 = SB_BFE(col2,   17,         3);
				bool  blend  = SB_BFE(bldcnt,  8 + type2, 1);
				if (blend) {
					UINT16 bldalpha = gba_io_read16(gba, GBA_BLDALPHA);
					INT32  r2  = SB_BFE(col2,     0, 5);
					INT32  g2  = SB_BFE(col2,     5, 5);
					INT32  b2  = SB_BFE(col2,    10, 5);
					INT32  eva = SB_BFE(bldalpha, 0, 5);
					INT32  evb = SB_BFE(bldalpha, 8, 5);
					if (eva > 16) eva = 16;
					if (evb > 16) evb = 16;
					r = (r * eva + r2 * evb) / 16;
					g = (g * eva + g2 * evb) / 16;
					b = (b * eva + b2 * evb) / 16;
					if (r > 31) r = 31;
					if (g > 31) g = 31;
					if (b > 31) b = 31;
				}
			}
				break;	//Alpha Blend
			case 2:		//Lighten
				r = r + (31 - r) * evy;
				g = g + (31 - g) * evy;
				b = b + (31 - b) * evy;
				break;
			case 3:		//Darken
				r = r - (r     ) * evy;
				g = g - (g     ) * evy;
				b = b - (b     ) * evy;
				break;
		}
	}
	if (forced_blank) {
		r = g = b = 255;
		if (gba->stop_mode)
			r = g = b = 0;
	}

	INT32  backdrop_type = 5;
	UINT32 backdrop_col  = (*(UINT16*)(gba->mem.palette + GBA_BG_PALETTE + 0 * 2)) | (backdrop_type << 17);
	gba->first_target_buffer[ lcd_x] = backdrop_col;
	gba->second_target_buffer[lcd_x] = backdrop_col;

	INT32  p = (lcd_x + lcd_y * 240) * 4;
	float  screen_blend_factor = 0.3 * gba->ppu.ghosting_strength;
	UINT16 green_swap = gba_io_read16(gba, GBA_GREENSWP);
	gba->framebuffer[p + 0] = r * 8 * (1.0 - screen_blend_factor) + gba->framebuffer[p + 0] * screen_blend_factor;
	gba->framebuffer[p + 2] = b * 8 * (1.0 - screen_blend_factor) + gba->framebuffer[p + 2] * screen_blend_factor;

	if (green_swap & 1) {
		if (p & 4)
			gba->framebuffer[p + 1 - 4] = g * 8 * (1.0 - screen_blend_factor) + gba->framebuffer[p + 1 - 4] * screen_blend_factor;
		else
			gba->framebuffer[p + 1 + 4] = g * 8 * (1.0 - screen_blend_factor) + gba->framebuffer[p + 1 + 4] * screen_blend_factor;
	} else {
		gba->framebuffer[p + 1] = g * 8 * (1.0 - screen_blend_factor) + gba->framebuffer[p + 1] * screen_blend_factor;
	}
}

// Scanline render: registers snapshotted at hblank start; pixel loop performs no IO reads
static inline void gba_ppu_render_scanline(gba_t* gba, INT32 lcd_y)
{
	UINT16 dispcnt      = gba_io_read16(gba, GBA_DISPCNT);
	INT32  bg_mode      = SB_BFE(dispcnt, 0, 3);
	INT32  forced_blank = SB_BFE(dispcnt, 7, 1);
	INT32  frame_sel    = SB_BFE(dispcnt, 4, 1);
	UINT16 mos_reg      = gba_io_read16(gba, GBA_MOSAIC);
	INT32  mos_x        = SB_BFE(mos_reg, 0, 4) + 1;
	INT32  mos_y        = SB_BFE(mos_reg, 4, 4) + 1;
	UINT16 bldcnt       = gba_io_read16(gba, GBA_BLDCNT);
	INT32  bld_mode     = SB_BFE(bldcnt, 6, 2);
	UINT16 bldy_reg     = gba_io_read16(gba, GBA_BLDY);
	UINT16 bldalpha     = gba_io_read16(gba, GBA_BLDALPHA);
	UINT16 green_swap   = gba_io_read16(gba, GBA_GREENSWP);
	UINT32 backdrop_col = (*(UINT16*)(gba->mem.palette + GBA_BG_PALETTE + 0 * 2)) | (5 << 17);
	float  sbf          = 0.3 * gba->ppu.ghosting_strength;
	float  evy          = SB_BFE(bldy_reg, 0, 5) / 16.;
	if (evy > 1.0)
		evy = 1;

	bool render_bgs  = bg_mode <= 5;
	bool mode_ok[4]  = { false, false, false, false };
	INT32 priority[4], char_addr[4], scr_addr[4], size_x[4], size_y[4], ssize[4];
	INT32 hoff[4], voff[4], bgx[4], bgy[4], pa[4], pc[4];
	bool colors[4], mosaic_bg[4], rot_scale[4], overflow[4];
	// text BG row constants + per-tile-column cache: tilemap read once per tile
	INT32 py0[4], trow_base[4], cached_tile_x[4], cached_py[4];
	UINT16 cached_tile_data[4];
	for (INT32 bg = 0; bg < 4; ++bg)
		cached_tile_x[bg] = -1;
	if (render_bgs) {
		for (INT32 bg = 0; bg < 4; ++bg) {
			if ((bg < 2 && bg_mode == 2) || (bg == 3 && bg_mode == 1) || (bg != 2 && bg_mode >= 3))
				continue;
			if (!SB_BFE(dispcnt, 8 + bg, 1))
				continue;
			mode_ok[bg]    = true;
			rot_scale[bg]  = bg_mode >= 1 && bg >= 2;
			UINT16 bgcnt   = gba_io_read16(gba, GBA_BG0CNT + bg * 2);
			priority[bg]   = SB_BFE(bgcnt,  0, 2);
			char_addr[bg]  = SB_BFE(bgcnt,  2, 2) * 16 * 1024;
			mosaic_bg[bg]  = SB_BFE(bgcnt,  6, 1);
			colors[bg]     = SB_BFE(bgcnt,  7, 1);
			scr_addr[bg]   = SB_BFE(bgcnt,  8, 5) * 2048;
			overflow[bg]   = SB_BFE(bgcnt, 13, 1);
			ssize[bg]      = SB_BFE(bgcnt, 14, 2);
			size_x[bg]     = (ssize[bg] & 1 ) ? 512 : 256;
			size_y[bg]     = (ssize[bg] >= 2) ? 512 : 256;
			if (rot_scale[bg]) {
				size_x[bg] = size_y[bg] = (16 * 8) << ssize[bg];
				if (bg_mode == 3 || bg_mode == 4) {
					size_x[bg] = 240;
					size_y[bg] = 160;
				} else if (bg_mode == 5) {
					size_x[bg] = 160;
					size_y[bg] = 128;
				}
				colors[bg] = true;
				bgx[bg] = gba->ppu.aff[bg - 2].render_bgx;
				bgy[bg] = gba->ppu.aff[bg - 2].render_bgy;
				pa[bg] = (INT16)gba_io_read16(gba, GBA_BG2PA + (bg - 2) * 0x10);
				pc[bg] = (INT16)gba_io_read16(gba, GBA_BG2PC + (bg - 2) * 0x10);
			} else {
				INT16 h16 = gba_io_read16(gba, GBA_BG0HOFS + bg * 4);
				INT16 v16 = gba_io_read16(gba, GBA_BG0VOFS + bg * 4);
				hoff[bg] = (h16 << 7) >> 7;
				voff[bg] = (v16 << 7) >> 7;
				INT32 ly     = mosaic_bg[bg] ? (lcd_y / mos_y) * mos_y : lcd_y;
				INT32 row_y  = (voff[bg] + ly) & (size_y[bg] - 1);
				py0[bg]      = row_y  & 7;
				INT32 ty     = row_y >> 3;
				trow_base[bg] = (ty & 31) * 32 + (ty >= 32 ? 32 * 32 * (ssize[bg] == 3 ? 2 : 1) : 0);
			}
		}
	}

	for (INT32 lcd_x = 0; lcd_x < 240; ++lcd_x) {
		UINT8 window_control = gba->window[lcd_x];
		if (render_bgs) {
			for (INT32 bg = 3; bg >= 0; --bg) {
				if (!mode_ok[bg] || SB_BFE(window_control, bg, 1) == 0)
					continue;
				UINT32 col  = 0;
				INT32  bg_x = 0;
				INT32  bg_y = 0;
				if (rot_scale[bg]) {
					INT32 sx = mosaic_bg[bg] ? (lcd_x / mos_x) * mos_x : lcd_x;
					INT64 x2 = (INT64)pa[bg] * sx + (((INT64)bgx[bg]));
					INT64 y2 = (INT64)pc[bg] * sx + (((INT64)bgy[bg]));
					bg_x = (INT32)(x2 >> 8);
					bg_y = (INT32)(y2 >> 8);
					if (overflow[bg] == 0) {
						if (bg_x < 0 || bg_x >= size_x[bg] || bg_y < 0 || bg_y >= size_y[bg])
							continue;
					} else {
						bg_x %= size_x[bg];
						bg_y %= size_y[bg];
					}
				} else {
					if (mosaic_bg[bg]) {
						bg_x = hoff[bg] + (lcd_x / mos_x) * mos_x;
						bg_y = voff[bg] + (lcd_y / mos_y) * mos_y;
					} else {
						bg_x = hoff[bg] + lcd_x;
						bg_y = voff[bg] + lcd_y;
					}
				}
				if (bg_mode == 3) {
					INT32 p    = bg_x + bg_y * 240;
					col = *(UINT16*)(gba->mem.vram + p * 2);
				} else if (bg_mode == 4) {
					INT32 p    = bg_x + bg_y * 240;
					INT32 addr = p + 0xa000 * frame_sel;
					UINT8 palette_id = gba->mem.vram[addr];
					if (palette_id == 0)
						continue;
					col = *(UINT16*)(gba->mem.palette + GBA_BG_PALETTE + palette_id * 2);
				} else if (bg_mode == 5) {
					INT32 p    = bg_x + bg_y * 160;
					INT32 addr = p * 2 + 0xa000 * frame_sel;
					col = *(UINT16*)(gba->mem.vram + addr);
				} else {
					bg_x = bg_x & (size_x[bg] - 1);
					INT32 bg_tile_x = bg_x >> 3;

					UINT16 tile_data;
					INT32 px, py;
					if (rot_scale[bg]) {
						INT32 tile_off = (bg_y >> 3) * (size_x[bg] >> 3) + bg_tile_x;
						tile_data = gba->mem.vram[scr_addr[bg] + tile_off];
						px = bg_x & 7;
						py = bg_y & 7;
					} else {
						if (bg_tile_x != cached_tile_x[bg]) {
							cached_tile_x[bg] = bg_tile_x;
							INT32 toff = trow_base[bg] + (bg_tile_x & 31) + (bg_tile_x >= 32 ? 32 * 32 : 0);
							tile_data = *(UINT16*)(gba->mem.vram + scr_addr[bg] + toff * 2);
							cached_tile_data[bg] = tile_data;
							cached_py[bg] = SB_BFE(tile_data, 11, 1) ? 7 - py0[bg] : py0[bg];
						} else
							tile_data = cached_tile_data[bg];
						px = bg_x & 7;
						if (SB_BFE(tile_data, 10, 1))
							px = 7 - px;
						py = cached_py[bg];
					}
					INT32 tile_id = SB_BFE(tile_data,  0, 10);
					INT32 palette = SB_BFE(tile_data, 12,  4);

					UINT8 tile_d = tile_id;
					if (colors[bg] == false) {
						INT32 addr = char_addr[bg] + tile_id * 8 * 4 + px / 2 + py * 4;
						tile_d = gba->mem.vram[addr];
						tile_d = (tile_d >> ((px & 1) * 4)) & 0xf;
						//There is an undocumented GBA quirk where tiles over 64KB are not loaded
						if (tile_d == 0 || SB_UNLIKELY(addr >= 0x10000))
							continue;
						tile_d += palette * 16;
					} else {
						//There is an undocumented GBA quirk where tiles over 64KB are not loaded
						INT32 addr = char_addr[bg] + tile_id * 8 * 8 + px + py * 8;
						tile_d = gba->mem.vram[addr];
						if (tile_d == 0 || SB_UNLIKELY(addr >= 0x10000))
							continue;
					}
					UINT8 palette_id = tile_d;
					col = *(UINT16*)(gba->mem.palette + GBA_BG_PALETTE + palette_id * 2);
				}
				col |= (bg << 17) | ((5 - priority[bg]) << 28) | ((4 - bg) << 25);
				if (col > gba->first_target_buffer[lcd_x]) {
					UINT32 t = gba->first_target_buffer[lcd_x];
					gba->first_target_buffer[lcd_x] = col;
					col = t;
				}
				if (col > gba->second_target_buffer[lcd_x])
					gba->second_target_buffer[lcd_x] = col;
			}
		}
		UINT32 col  = gba->first_target_buffer[lcd_x];
		INT32  r    = SB_BFE(col,  0, 5);
		INT32  g    = SB_BFE(col,  5, 5);
		INT32  b    = SB_BFE(col, 10, 5);
		UINT32 type = SB_BFE(col, 17, 3);

		INT32 mode = bld_mode;
		bool  effect_enable = SB_BFE(window_control, 5, 1);

		//Semitransparent objects are always selected for blending
		if (SB_BFE(col, 16, 1)) {
			UINT32 col2  = gba->second_target_buffer[lcd_x];
			UINT32 type2 = SB_BFE(col2,   17,         3);
			bool   blend = SB_BFE(bldcnt,  8 + type2, 1);
			if (blend) {
				mode          = 1;
				effect_enable = true;
			} else effect_enable &= SB_BFE(bldcnt, type, 1);
		} else effect_enable &= SB_BFE(bldcnt, type, 1);
		if (effect_enable) {
			switch (mode) {
				case 0:
					break;	//None
				case 1: {
					UINT32 col2  = gba->second_target_buffer[lcd_x];
					UINT32 type2 = SB_BFE(col2,  17,         3);
					bool  blend  = SB_BFE(bldcnt, 8 + type2, 1);
					if (blend) {
						INT32 r2  = SB_BFE(col2,     0, 5);
						INT32 g2  = SB_BFE(col2,     5, 5);
						INT32 b2  = SB_BFE(col2,    10, 5);
						INT32 eva = SB_BFE(bldalpha, 0, 5);
						INT32 evb = SB_BFE(bldalpha, 8, 5);
						if (eva > 16) eva = 16;
						if (evb > 16) evb = 16;
						r = (r * eva + r2 * evb) / 16;
						g = (g * eva + g2 * evb) / 16;
						b = (b * eva + b2 * evb) / 16;
						if (r > 31) r = 31;
						if (g > 31) g = 31;
						if (b > 31) b = 31;
					}
				}
					break;	//Alpha Blend
				case 2:		//Lighten
					r = r + (31 - r) * evy;
					g = g + (31 - g) * evy;
					b = b + (31 - b) * evy;
					break;
				case 3:		//Darken
					r = r - (r     ) * evy;
					g = g - (g     ) * evy;
					b = b - (b     ) * evy;
					break;
			}
		}
		if (forced_blank) {
			r = g = b = 255;
			if (gba->stop_mode)
				r = g = b = 0;
		}

		gba->first_target_buffer[ lcd_x] = backdrop_col;
		gba->second_target_buffer[lcd_x] = backdrop_col;

		INT32 p = (lcd_x + lcd_y * 240) * 4;
		gba->framebuffer[p + 0] = r * 8 * (1.0 - sbf) + gba->framebuffer[p + 0] * sbf;
		gba->framebuffer[p + 2] = b * 8 * (1.0 - sbf) + gba->framebuffer[p + 2] * sbf;

		if (green_swap & 1) {
			if (p & 4)
				gba->framebuffer[p + 1 - 4] = g * 8 * (1.0 - sbf) + gba->framebuffer[p + 1 - 4] * sbf;
			else
				gba->framebuffer[p + 1 + 4] = g * 8 * (1.0 - sbf) + gba->framebuffer[p + 1 + 4] * sbf;
		} else {
			gba->framebuffer[p + 1] = g * 8 * (1.0 - sbf) + gba->framebuffer[p + 1] * sbf;
		}
	}
}

// On-read refresh of DISPSTAT/VCOUNT from the current beam position; internal reads are not routed here
static inline void gba_ppu_refresh_status(gba_t* gba)
{
	if (!gba->ppu_event.active)
		return;
	INT32 remaining = (INT32)(gba->ppu_event.when - gba->global_timer);
	if (remaining < 0)
		remaining = 0;
	INT32 beam = (INT32)gba->ppu.scan_clock - remaining;
	if (beam < 0)
		beam += 280896;
	INT32 lcd_y  = beam / 1232;
	INT32 lcd_x  = (beam % 1232) / 4;
	INT32 vcount = (lcd_y + (lcd_x >= GBA_LCD_HBLANK_END)) % 228;
	bool  vblank = lcd_y >= 160 && lcd_y < 227;
	bool  hblank = lcd_x >= GBA_LCD_HBLANK_START && lcd_x < GBA_LCD_HBLANK_END;
	UINT16 disp_stat = gba_io_read16(gba, GBA_DISPSTAT) & ~0x7;
	disp_stat |= vblank ? 0x1 : 0;
	disp_stat |= hblank ? 0x2 : 0;
	disp_stat |= vcount == SB_BFE(disp_stat, 8, 8) ? 0x4 : 0;
	gba_io_store16(gba, GBA_DISPSTAT, disp_stat);
	gba_io_store16(gba, GBA_VCOUNT,   vcount);
}

static inline void gba_ppu_event(gba_t* gba, sb_emu_state_t* emu, UINT32 cycles_late)
{
	bool render = emu->render_frame;
	if (gba->ppu.scan_clock >= 280896)
		gba->ppu.scan_clock -= 280896;
	INT32 lcd_y = ( gba->ppu.scan_clock) / 1232;
	INT32 lcd_x = ((gba->ppu.scan_clock) % 1232) / 4;
	gba->ppu.scan_clock++;
	INT32 fast_forward_ticks = gba_ppu_compute_max_fast_forward(gba, render && gba->ppu.render_per_pixel) + 1;
	gba->ppu.scan_clock += fast_forward_ticks;
	if (lcd_x == 0 || lcd_x == GBA_LCD_HBLANK_START || lcd_x == GBA_LCD_HBLANK_END) {
		UINT16 disp_stat  = gba_io_read16(gba, GBA_DISPSTAT) & ~0x7;
		UINT16 vcount_cmp = SB_BFE(disp_stat, 8, 8);
		INT32  vcount = (lcd_y + (lcd_x >= GBA_LCD_HBLANK_END)) % 228;
		bool   vblank = lcd_y >= 160 && lcd_y < 227;
		bool   hblank = lcd_x >= GBA_LCD_HBLANK_START && lcd_x < GBA_LCD_HBLANK_END;
		disp_stat |= vblank ? 0x1 : 0;
		disp_stat |= hblank ? 0x2 : 0;
		disp_stat |= vcount == vcount_cmp ? 0x4 : 0;
		gba_io_store16(gba, GBA_DISPSTAT, disp_stat);
		gba_io_store16(gba, GBA_VCOUNT  , vcount);
		UINT32 new_if = 0;
		if (hblank != gba->ppu.last_hblank) {
			gba->ppu.last_hblank = hblank;
			bool hblank_irq_en   = SB_BFE(disp_stat, 4, 1);
			if (hblank && hblank_irq_en)
				new_if |= (1 << GBA_INT_LCD_HBLANK);
			if (hblank) {
				// DMA wake 2 cycles after the edge (hardware startup delay)
				++gba->ppu.hblank_seq;
				gba_timing_schedule(gba, &gba->dma_event, 2);
			}
			if (!hblank) {
				gba->ppu.dispcnt_pipeline[0] = gba->ppu.dispcnt_pipeline[1];
				gba->ppu.dispcnt_pipeline[1] = gba->ppu.dispcnt_pipeline[2];
				gba->ppu.dispcnt_pipeline[2] = gba_io_read16(gba, GBA_DISPCNT);
			}
		}
		if (lcd_y != gba->ppu.last_lcd_y) {
			if (vblank != gba->ppu.last_vblank) {
				if (vblank) {
					gba->frame_in_progress = false;
					++gba->ppu.vblank_seq;
					gba_timing_schedule(gba, &gba->dma_event, 2);
				}
				gba->ppu.last_vblank = vblank;
				bool vblank_irq_en = SB_BFE(disp_stat, 3, 1);
				if (vblank && vblank_irq_en)
					new_if |= (1 << GBA_INT_LCD_VBLANK);
			}
			gba->ppu.last_lcd_y = lcd_y;
			if (lcd_y == vcount_cmp) {
				bool vcnt_irq_en = SB_BFE(disp_stat, 5, 1);
				if (vcnt_irq_en)
					new_if |= (1 << GBA_INT_LCD_VCOUNT);
			}
		}
		gba_send_interrupt(gba, 3, new_if);
	}

	if (!render) {
		// skip frames: no pixel work, but the event must stay scheduled
		gba_timing_deschedule(gba, &gba->ppu_event);
		gba_timing_schedule(gba, &gba->ppu_event,
		                    fast_forward_ticks + 1 - (INT32)cycles_late);
		return;
	}

	if (lcd_x == 0) {
		// Affine reference updated at scanline start so the per-line increment uses
		// the CURRENT line's PB/PD (sampled ~cycle 6)
		UINT16 dispcnt = gba->ppu.dispcnt_pipeline[0];
		INT32  bg_mode = SB_BFE(dispcnt, 0, 3);
		// Line 0 skips the increment and reloads from BG2X/BG2Y
		if (bg_mode != 0 && lcd_y != 0) {
			for (INT32 aff = 0; aff < 2; ++aff) {
				bool bg_en = SB_BFE(dispcnt, 8 + aff + 2, 1);
				if (!bg_en)
					continue;
				INT32  b = (INT16)gba_io_read16(gba, GBA_BG2PB  + (aff) * 0x10);
				INT32  d = (INT16)gba_io_read16(gba, GBA_BG2PD  + (aff) * 0x10);
				UINT16 bgcnt = gba_io_read16(gba, GBA_BG2CNT +  aff  * 2);
				bool mosaic = SB_BFE(bgcnt, 6, 1);
				if (mosaic) {
					UINT16 mos_reg = gba_io_read16(gba, GBA_MOSAIC);
					INT32  mos_y   = SB_BFE(mos_reg, 4, 4) + 1;
					if ((lcd_y % mos_y) == 0) {
						gba->ppu.aff[aff].render_bgx += b * mos_y;
						gba->ppu.aff[aff].render_bgy += d * mos_y;
					}
				} else {
					gba->ppu.aff[aff].render_bgx += b;
					gba->ppu.aff[aff].render_bgy += d;
				}
			}
		}
		// Reload from BG2X/BG2Y when written this scanline, or unconditionally on line 0
		for (INT32 aff = 0; aff < 2; ++aff) {
			if (gba->ppu.aff[aff].wrote_bgx || lcd_y == 0) {
				gba->ppu.aff[aff].render_bgx = gba_io_read32(gba, GBA_BG2X + (aff) * 0x10);
				gba->ppu.aff[aff].render_bgx = SB_BFE(gba->ppu.aff[aff].render_bgx, 0, 28);
				gba->ppu.aff[aff].render_bgx = ((INT32)(gba->ppu.aff[aff].render_bgx << 4)) >> 4;
				gba->ppu.aff[aff].wrote_bgx  = false;
			}
			if (gba->ppu.aff[aff].wrote_bgy || lcd_y == 0) {
				gba->ppu.aff[aff].render_bgy = gba_io_read32(gba, GBA_BG2Y + (aff) * 0x10);
				gba->ppu.aff[aff].render_bgy = SB_BFE(gba->ppu.aff[aff].render_bgy, 0, 28);
				gba->ppu.aff[aff].render_bgy = ((INT32)(gba->ppu.aff[aff].render_bgy << 4)) >> 4;
				gba->ppu.aff[aff].wrote_bgy  = false;
			}
		}
	}
	if (gba->ppu.render_per_pixel) {
		//Render sprites over scanline when it completes
		if ((lcd_y < 159 || lcd_y == 227) && lcd_x == GBA_LCD_HBLANK_START)
			gba_ppu_render_objs(gba, (lcd_y + 1) % 228);
		if (lcd_x < 240 && lcd_y < 160)
			gba_ppu_render_pixel(gba, lcd_x, lcd_y);
	} else if (lcd_y < 160 && lcd_x == GBA_LCD_HBLANK_START) {
		// Composite the row at hblank start before hblank DMA/IRQ take effect
		gba_ppu_render_objs(gba, lcd_y);
		gba_ppu_render_scanline(gba, lcd_y);
	}

	// next boundary: keep the absolute scanline grid regardless of dispatch lateness
	gba_timing_deschedule(gba, &gba->ppu_event);
	gba_timing_schedule(gba, &gba->ppu_event, fast_forward_ticks + 1 - (INT32)cycles_late);
}

// ============================================================================
// Multi-threaded PPU renderer: worker thread + per-scanline snapshots
// ============================================================================
// Uncomment the next line to force-disable multi-threading (single-thread fallback).
// #define GBA_DISABLE_MT

// ---- Platform detection ---------------------------------------------------
#if !defined(GBA_DISABLE_MT)

#  if defined(__unix__) || defined(__APPLE__) || defined(__ANDROID__) || \
      defined(__linux__) || defined(__HAIKU__)
#    include <pthread.h>
#    include <semaphore.h>
#    include <fcntl.h>
#    include <errno.h>
#    include <stdio.h>
#    define GBA_HAVE_PTHREAD   1
#    define GBA_HAVE_WINTHREAD 0

#  elif defined(_WIN32) || defined(BUILD_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#      define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#      define NOMINMAX
#    endif
#    include <windows.h>
#    define GBA_HAVE_PTHREAD   0
#    define GBA_HAVE_WINTHREAD 1
#  else
#    define GBA_HAVE_PTHREAD   0
#    define GBA_HAVE_WINTHREAD 0
#  endif
#else
#  define GBA_HAVE_PTHREAD   0
#  define GBA_HAVE_WINTHREAD 0
#endif

#if !defined(GBA_HAVE_PTHREAD)
#  define GBA_HAVE_PTHREAD   0
#endif
#if !defined(GBA_HAVE_WINTHREAD)
#  define GBA_HAVE_WINTHREAD 0
#endif

#define GBA_MT_ENABLED (GBA_HAVE_PTHREAD || GBA_HAVE_WINTHREAD)

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
// Multi-threaded path (POSIX pthreads or Win32 threads available)
// ===========================================================================
#if GBA_MT_ENABLED

// MMIO window captured per scanline: DISPCNT..GREENSWP (0x00..0x58 = 89 bytes,
// rounded up to 96).  Covers every PPU register the render path reads.
#define GBA_MT_PPU_IO_SIZE  0x60

// Triple-buffered pipeline:
//   Normal play       — blocking wait for lowest latency (~2-slot effective)
//   Fast-forward/skip — non-blocking trywait, drop frames if worker is busy
#define GBA_MT_N_BUFFERS    3

#if GBA_MT_N_BUFFERS < 2 || GBA_MT_N_BUFFERS > 8
#error GBA_MT_N_BUFFERS must be between 2 and 8
#endif

// ---- Data structures ------------------------------------------------------

// Per-scanline packed state: MMIO + affine reference points + DISPCNT pipeline.
// Packed into one struct so the worker installs a row in a single memcpy
// instead of 6+ scattered assignments, and the snapshot side writes once.
typedef struct {
	UINT8  io[GBA_MT_PPU_IO_SIZE];
	INT32  bgx[2];                        // BG2/BG3 affine reference X
	INT32  bgy[2];                        // BG2/BG3 affine reference Y
	UINT16 dispcnt_pipeline[3];           // 3-stage DISPCNT delay pipeline
} gba_mt_line_state_t;

// Per-frame snapshot handed to the worker.
// Main thread writes snapshots[w_idx]; worker reads snapshots[r_idx].
// The worker writes its pixel output into backbuf[r_idx] — snapshot slot and
// backbuf slot share the same index (1:1 mapping), which simplifies invariants
// and means a slot is either "being captured by main", "being rendered by
// worker", or "on screen / free for reuse", never all three at once.
typedef struct {
	UINT8  vram[128 * 1024];
	UINT8  oam[1024];
	UINT8  palette[1024];

	gba_mt_line_state_t line_state[GBA_LCD_H];
	bool   line_hb_valid[GBA_LCD_H];      // hblank snapshot captured for row
	bool   line_ls_valid[GBA_LCD_H];      // line-start snapshot captured
	bool   vram_copied;                   // big-buffer copy done this frame

	float  ghosting_strength;
	bool   render_per_pixel;
} gba_mt_snapshot_t;

// MT control block.
// Cross-thread ordering is provided exclusively by:
//   - sem_wait (acquire) / sem_post (release) pairs on job_sem / done_sem
//   - explicit full memory barriers (gba_mt_plat_mb) before each post
// Volatile is used on fields shared between threads purely to prevent the
// compiler from caching them in registers across semaphore calls; the real
// visibility guarantee comes from the acquire/release semantics.
typedef struct gba_mt_s {
	bool enabled;
	bool exiting;

	gba_mt_snapshot_t snapshots[GBA_MT_N_BUFFERS];
	UINT8  backbuf[GBA_MT_N_BUFFERS][GBA_LCD_W * GBA_LCD_H * 4];

	volatile int w_idx;           // slot main thread is currently writing
	volatile int r_idx;           // slot worker is currently reading/writing
	volatile int ready_back_idx;  // last backbuf the worker completed
	volatile int front_idx;       // backbuf currently presented on screen
	int          presented_front_idx; // front idx last copied to the framebuffer
	bool         presented_black;     // black frame already presented
	UINT32       frame_count;

	volatile bool want_render_this_frame;
	volatile bool worker_busy;
	volatile bool job_pending;
	volatile bool have_first_frame;
	volatile bool fast_forward;   // true while main thread is skipping renders

#if GBA_HAVE_PTHREAD
	pthread_t      worker;
	sem_t*         job_sem;
	sem_t*         done_sem;
	char           job_sem_name[40];
	char           done_sem_name[40];
#elif GBA_HAVE_WINTHREAD
	HANDLE         worker;
	HANDLE         job_sem;
	HANDLE         done_sem;
#endif
} gba_mt_t;

extern gba_mt_t* g_gba_mt_current;

// ---- Public API -----------------------------------------------------------
static INT32  gba_mt_init(gba_mt_t* mt);
static void   gba_mt_exit(gba_mt_t* mt);
static void   gba_mt_reset(gba_mt_t* mt);
static void   gba_mt_begin_frame(gba_mt_t* mt, gba_t* gba, sb_emu_state_t* emu);
static void   gba_mt_ppu_hblank_snapshot(gba_mt_t* mt, gba_t* gba, INT32 lcd_y);
static void   gba_mt_ppu_line_start_snapshot(gba_mt_t* mt, gba_t* gba, INT32 lcd_y);
static void   gba_mt_ppu_vblank_entry(gba_mt_t* mt, gba_t* gba);
static void   gba_mt_ppu_vblank_publish(gba_mt_t* mt, gba_t* gba, gba_scratch_t* scratch);

static inline bool gba_mt_enabled(const gba_mt_t* mt) {
	return mt && mt->enabled;
}

// ---- reset_slot -----------------------------------------------------------
// Fill a snapshot slot with safe VBlank-time defaults.  If any scanline's
// hblank / line-start snapshot is missed during fast-forward / frame-skip,
// the row still has valid MMIO values rather than zeroes (all-zero DISPCNT
// means all layers off → black scanlines).
static void gba_mt_reset_slot(gba_mt_t* mt, int idx, gba_t* gba)
{
	gba_mt_snapshot_t* s = &mt->snapshots[idx];
	memset(s->line_hb_valid, 0, sizeof(s->line_hb_valid));
	memset(s->line_ls_valid, 0, sizeof(s->line_ls_valid));
	s->vram_copied       = false;
	s->ghosting_strength = gba->ppu.ghosting_strength;
	s->render_per_pixel  = gba->ppu.render_per_pixel;

	// Capture VBlank-time MMIO + affine + pipeline once outside the loop.
	const UINT8* io_base = gba->mem.io + (GBA_DISPCNT & 0xfff);
	gba_mt_line_state_t seed;
	memcpy(seed.io, io_base, GBA_MT_PPU_IO_SIZE);
	for (int aff = 0; aff < 2; ++aff) {
		seed.bgx[aff] = gba->ppu.aff[aff].render_bgx;
		seed.bgy[aff] = gba->ppu.aff[aff].render_bgy;
	}
	for (int p = 0; p < 3; ++p)
		seed.dispcnt_pipeline[p] = gba->ppu.dispcnt_pipeline[p];

	// Broadcast the seed to all 160 rows, 4-at-a-time unrolled for a
	// little ILP on the main thread.
	INT32 y = 0;
	for (; y + 3 < GBA_LCD_H; y += 4) {
		s->line_state[y+0] = seed;
		s->line_state[y+1] = seed;
		s->line_state[y+2] = seed;
		s->line_state[y+3] = seed;
	}
	for (; y < GBA_LCD_H; ++y)
		s->line_state[y] = seed;
}

// ---- install_row ----------------------------------------------------------
// Install a single row's line_state into the worker's scratch gba_t and
// remember it for fallback use when a later row's snapshot is missing.
static inline void gba_mt_install_row(gba_t* gba, const gba_mt_line_state_t* ls,
                                       int row_y, gba_mt_line_state_t* installed_prev,
                                       int* prev_valid_row)
{
	(void)row_y;
	memcpy(gba->mem.io + (GBA_DISPCNT & 0xfff), ls->io, GBA_MT_PPU_IO_SIZE);
	for (int aff = 0; aff < 2; ++aff) {
		gba->ppu.aff[aff].render_bgx = ls->bgx[aff];
		gba->ppu.aff[aff].render_bgy = ls->bgy[aff];
		gba->ppu.aff[aff].wrote_bgx  = false;
		gba->ppu.aff[aff].wrote_bgy  = false;
	}
	for (int p = 0; p < 3; ++p)
		gba->ppu.dispcnt_pipeline[p] = ls->dispcnt_pipeline[p];
	*installed_prev  = *ls;
	*prev_valid_row  = row_y;
}

// ---- gba_mt_render_frame_into ---------------------------------------------
// Worker entry point for pixel composition.  Reads *only* from snap, writes
// *only* to out_fb.  Uses a stack-local gba_t so no main-thread state is
// touched during rendering.
static void gba_mt_render_frame_into(gba_mt_snapshot_t* snap, UINT32* out_fb,
                                     UINT32* prev_fb_for_ghosting)
{
	gba_t gba;
	// Zero the entire stack gba_t (~155 KB).  Runs on the worker thread
	// (parallel with main-thread emulation) so its cost is not on the hot
	// path.  Zeroing guarantees deterministic behaviour for any fields
	// the render path touches that we don't explicitly initialise below,
	// preventing UB from uninitialised stack contents.
	memset(&gba, 0, sizeof(gba));

	// Big buffers — copied by main thread at VBlank entry, BEFORE any
	// VBlank DMA has a chance to overwrite the live buffers.
	memcpy(gba.mem.vram,    snap->vram,    sizeof(snap->vram));
	memcpy(gba.mem.oam,     snap->oam,     sizeof(snap->oam));
	memcpy(gba.mem.palette, snap->palette, sizeof(snap->palette));
	gba.ppu.ghosting_strength = snap->ghosting_strength;
	gba.ppu.render_per_pixel  = snap->render_per_pixel;
	gba.framebuffer           = (UINT8*)out_fb;

	// Ghosting seed: copy the previous displayed frame into the output
	// buffer so the per-pixel rasteriser can blend new pixels on top
	// using SBF — exactly matching single-thread behaviour.
	// We do NOT pre-attenuate here; the raster already applies SBF once.
	// Pre-attenuating would double-apply (sbf * sbf) making ghosting
	// weaker and darker than intended.
	{
		const INT32 total_bytes = GBA_LCD_W * GBA_LCD_H * 4;
		if (snap->ghosting_strength > 0.0f && prev_fb_for_ghosting) {
			memcpy(out_fb, prev_fb_for_ghosting, total_bytes);
		} else {
			memset(out_fb, 0, total_bytes);
		}
	}

	// Backdrop initialisation: palette[0] with target type=5 so pixels
	// with no layer coverage blend correctly through BLDCNT/BLDY.
	{
		UINT16 bg_pal0 = *(const UINT16*)(gba.mem.palette + GBA_BG_PALETTE);
		UINT32 backdrop_col = ((UINT32)bg_pal0) | (5u << 17);

		INT32 x = 0;
		for (; x + 7 < GBA_LCD_W; x += 8) {
			gba.first_target_buffer[x+0] = backdrop_col;
			gba.first_target_buffer[x+1] = backdrop_col;
			gba.first_target_buffer[x+2] = backdrop_col;
			gba.first_target_buffer[x+3] = backdrop_col;
			gba.first_target_buffer[x+4] = backdrop_col;
			gba.first_target_buffer[x+5] = backdrop_col;
			gba.first_target_buffer[x+6] = backdrop_col;
			gba.first_target_buffer[x+7] = backdrop_col;
		}
		for (; x < GBA_LCD_W; ++x) gba.first_target_buffer[x] = backdrop_col;

		x = 0;
		for (; x + 7 < GBA_LCD_W; x += 8) {
			gba.second_target_buffer[x+0] = backdrop_col;
			gba.second_target_buffer[x+1] = backdrop_col;
			gba.second_target_buffer[x+2] = backdrop_col;
			gba.second_target_buffer[x+3] = backdrop_col;
			gba.second_target_buffer[x+4] = backdrop_col;
			gba.second_target_buffer[x+5] = backdrop_col;
			gba.second_target_buffer[x+6] = backdrop_col;
			gba.second_target_buffer[x+7] = backdrop_col;
		}
		for (; x < GBA_LCD_W; ++x) gba.second_target_buffer[x] = backdrop_col;

		// Window mask: all 6 layers enabled by default (0x3F);
		// render_objs / window logic will overwrite per-pixel.
		memset(gba.window, 0x3F, sizeof(gba.window));
	}

	const bool render_per_pixel = snap->render_per_pixel;

	// Last fully-installed line_state — used as fallback when a row's
	// snapshot is missing (fast-forward edge case).  Carrying the
	// previous valid state forward matches real hardware: MMIO/affine
	// reference points hold their last-written value until the CPU
	// rewrites them on a later scanline.
	gba_mt_line_state_t installed_prev;
	int prev_valid_row = -1;

	// Install line 0 (always valid — reset_slot prefills every row).
	gba_mt_install_row(&gba, &snap->line_state[0], 0,
	                    &installed_prev, &prev_valid_row);

	// Initialise mosaic counter at frame start (matches ST; both modes
	// rely on it being 0 at the top of the frame).
	gba.ppu.mosaic_y_counter = 0;

	if (render_per_pixel) {
		gba_ppu_render_objs(&gba, 0);
	}

	for (INT32 lcd_y = 0; lcd_y < GBA_LCD_H; ++lcd_y) {
		const gba_mt_line_state_t* ls = &snap->line_state[lcd_y];
		bool row_ok = snap->line_hb_valid[lcd_y] && snap->line_ls_valid[lcd_y];

		// Per-pixel mode reinstalls every row (render_pixel reads the
		// IO state set at the top of the iteration).  Scanline mode
		// already has row 0 installed above; reinstall for y>=1.
		if (lcd_y > 0 || render_per_pixel) {
			const gba_mt_line_state_t* ls_use;
			if (row_ok) {
				ls_use = ls;
			} else if (prev_valid_row >= 0) {
				ls_use = &installed_prev;
			} else {
				ls_use = &snap->line_state[0];  // last-resort VBlank seed
			}
			gba_mt_install_row(&gba, ls_use, lcd_y,
			                    &installed_prev, &prev_valid_row);
		}

		if (render_per_pixel) {
			// Per-pixel: first_target_buffer already has this line's
			// OBJs (preloaded at loop head for y=0, post-render for y>=1).
			INT32 lcd_x = 0;
			for (; lcd_x + 7 < GBA_LCD_W; lcd_x += 8) {
				gba_ppu_render_pixel(&gba, lcd_x+0, lcd_y);
				gba_ppu_render_pixel(&gba, lcd_x+1, lcd_y);
				gba_ppu_render_pixel(&gba, lcd_x+2, lcd_y);
				gba_ppu_render_pixel(&gba, lcd_x+3, lcd_y);
				gba_ppu_render_pixel(&gba, lcd_x+4, lcd_y);
				gba_ppu_render_pixel(&gba, lcd_x+5, lcd_y);
				gba_ppu_render_pixel(&gba, lcd_x+6, lcd_y);
				gba_ppu_render_pixel(&gba, lcd_x+7, lcd_y);
			}
			for (; lcd_x < GBA_LCD_W; ++lcd_x)
				gba_ppu_render_pixel(&gba, lcd_x, lcd_y);

			// Preload OBJs for the NEXT line (matches ST's y-HBlank
			// timing).  Temporarily install next line's IO so WIN/MOSAIC
			// registers are correct for that preload.  wrote_bgx/bgy
			// are cleared defensively so affine state never leaks
			// between rows.  The outer loop reinstalls the correct
			// row state at the top of its next iteration.
			if (lcd_y < GBA_LCD_H - 1) {
				INT32 ny = lcd_y + 1;
				bool next_ok = snap->line_hb_valid[ny] && snap->line_ls_valid[ny];
				const gba_mt_line_state_t* lsnext = next_ok
					? &snap->line_state[ny]
					: (prev_valid_row >= 0 ? &installed_prev : &snap->line_state[0]);
				memcpy(gba.mem.io + (GBA_DISPCNT & 0xfff),
				       lsnext->io, GBA_MT_PPU_IO_SIZE);
				for (int aff = 0; aff < 2; ++aff) {
					gba.ppu.aff[aff].render_bgx = lsnext->bgx[aff];
					gba.ppu.aff[aff].render_bgy = lsnext->bgy[aff];
					gba.ppu.aff[aff].wrote_bgx  = false;
					gba.ppu.aff[aff].wrote_bgy  = false;
				}
				for (int p = 0; p < 3; ++p)
					gba.ppu.dispcnt_pipeline[p] = lsnext->dispcnt_pipeline[p];
				gba_ppu_render_objs(&gba, ny);
			}
		} else {
			// Scanline mode: OBJ preload + BG composition in one shot
			// at HBlank-START timing, matching ST.
			gba_ppu_render_objs(&gba, lcd_y);
			gba_ppu_render_scanline(&gba, lcd_y);
		}
	}
}

// ---- begin_frame ----------------------------------------------------------
static void gba_mt_begin_frame(gba_mt_t* mt, gba_t* gba, sb_emu_state_t* emu)
{
	if (!mt || !mt->enabled) return;
	bool want = (emu && emu->render_frame);
	mt->fast_forward           = !want;
	mt->want_render_this_frame = want;
	// Prefill the write slot with VBlank defaults only on render frames;
	// skip frames (frame-skip / fast-forward) never render this slot.
	if (want)
		gba_mt_reset_slot(mt, mt->w_idx, gba);
}

// ---- HBlank MMIO snapshot -------------------------------------------------
static void gba_mt_ppu_hblank_snapshot(gba_mt_t* mt, gba_t* gba, INT32 lcd_y)
{
	if (!mt || !mt->enabled) return;
	if (lcd_y < 0 || lcd_y >= GBA_LCD_H) return;
	// On skipped frames reset_slot already seeded every row; save the
	// 15 KB of per-HBlank memcpy.
	if (!mt->want_render_this_frame) return;

	gba_mt_snapshot_t* snap = &mt->snapshots[mt->w_idx];
	memcpy(snap->line_state[lcd_y].io,
	       gba->mem.io + (GBA_DISPCNT & 0xfff), GBA_MT_PPU_IO_SIZE);
	snap->line_hb_valid[lcd_y] = true;
}

// ---- line-start snapshot (pipeline + affine, AFTER affine update) ---------
static void gba_mt_ppu_line_start_snapshot(gba_mt_t* mt, gba_t* gba, INT32 lcd_y)
{
	if (!mt || !mt->enabled) return;
	if (lcd_y < 0 || lcd_y >= GBA_LCD_H) return;
	if (!mt->want_render_this_frame) return;

	gba_mt_snapshot_t* snap = &mt->snapshots[mt->w_idx];
	gba_mt_line_state_t* ls = &snap->line_state[lcd_y];
	for (int aff = 0; aff < 2; ++aff) {
		ls->bgx[aff] = gba->ppu.aff[aff].render_bgx;
		ls->bgy[aff] = gba->ppu.aff[aff].render_bgy;
	}
	for (int p = 0; p < 3; ++p)
		ls->dispcnt_pipeline[p] = gba->ppu.dispcnt_pipeline[p];
	snap->line_ls_valid[lcd_y] = true;
}

// ---- VBlank entry (at VBlank edge, BEFORE VBlank DMA fires) ---------------
static void gba_mt_ppu_vblank_entry(gba_mt_t* mt, gba_t* gba)
{
	if (!mt || !mt->enabled) return;
	// only render frames need the big-buffer snapshot; skip frames would
	// copy ~130 KB the worker never renders (reset_slot recaptures on the
	// next render frame anyway)
	if (!mt->want_render_this_frame) return;

	gba_mt_snapshot_t* snap = &mt->snapshots[mt->w_idx];
	// On skipped frames where vram_copied is already true from a
	// previous frame (reset_slot sets it false on first entry after
	// publish), save the ~130 KB big-buffer memcpy — the worker isn't
	// going to render this slot anyway.
	if (snap->vram_copied) return;

	memcpy(snap->vram,    gba->mem.vram,    sizeof(snap->vram));
	memcpy(snap->oam,     gba->mem.oam,     sizeof(snap->oam));
	memcpy(snap->palette, gba->mem.palette, sizeof(snap->palette));
	snap->ghosting_strength = gba->ppu.ghosting_strength;
	snap->render_per_pixel  = gba->ppu.render_per_pixel;
	snap->vram_copied       = true;
}

// ===========================================================================
// MT-aware PPU event hook
//
// Runs the PPU state machine bit-exact with ST: DISPSTAT / VCOUNT / IRQs /
// pipeline shift / affine reference-point updates / DMA scheduling / MT
// snapshot capture.
//
// CRITICAL: pixel composition is NOT called here.  It runs exclusively in
// the worker via gba_mt_render_frame_into.
// ===========================================================================
static inline void gba_ppu_event_mt(gba_t* gba, sb_emu_state_t* emu, UINT32 cycles_late)
{
	bool render = emu ? emu->render_frame : true;

	if (gba->ppu.scan_clock >= 280896)
		gba->ppu.scan_clock -= 280896;

	INT32 lcd_y = (INT32)(gba->ppu.scan_clock / 1232);
	INT32 lcd_x = (INT32)((gba->ppu.scan_clock % 1232) / 4);
	gba->ppu.scan_clock++;

	INT32 fast_forward_ticks =
		gba_ppu_compute_max_fast_forward(gba, render && gba->ppu.render_per_pixel) + 1;

	// Advance scan_clock in lock-step with the global scheduler, even
	// across large skips (HBlank interior, VBlank/not-visible region).
	// Without this, fast-forward leaves scan_clock behind, lcd_y/lcd_x
	// go stale, and affine / snapshot events land on the wrong rows
	// → bottom-of-screen black blocks.
	gba->ppu.scan_clock += fast_forward_ticks;

	// Only evaluate DISPSTAT / edge events at the three column
	// boundaries ST cares about (lcd_x == 0 / HBLANK_START / HBLANK_END).
	// After a scan_clock jump the callback fires at an arbitrary dot;
	// evaluating edges in between would fire IRQs / snapshots multiple
	// times per edge.
	const bool at_column_edge =
		(lcd_x == 0) ||
		(lcd_x == GBA_LCD_HBLANK_START) ||
		(lcd_x == GBA_LCD_HBLANK_END);

	bool hblank = (lcd_x >= GBA_LCD_HBLANK_START) && (lcd_x < GBA_LCD_HBLANK_END);
	bool vblank = (lcd_y >= GBA_LCD_H) && (lcd_y < 227);
	INT32 vcount = (lcd_y + (lcd_x >= GBA_LCD_HBLANK_END)) % 228;

	UINT32 new_if = 0;
	if (at_column_edge) {
		UINT32 vcount_cmp = gba_io_read16(gba, GBA_DISPSTAT) >> 8;
		UINT16 disp_stat  = (UINT16)(gba_io_read16(gba, GBA_DISPSTAT) & ~0b111);
		if (vblank)                      disp_stat |= 1;
		if (hblank)                      disp_stat |= 2;
		if (vcount == (INT32)vcount_cmp) disp_stat |= 4;
		gba_io_store16(gba, GBA_DISPSTAT, disp_stat);
		gba_io_store16(gba, GBA_VCOUNT,  (UINT16)vcount);

		bool hblank_irq_en = SB_BFE(gba_io_read16(gba, GBA_DISPSTAT), 4, 1);
		if (hblank != gba->ppu.last_hblank) {
			gba->ppu.last_hblank = hblank;
			if (hblank) {
				if (hblank_irq_en)
					new_if |= (1 << GBA_INT_LCD_HBLANK);
				++gba->ppu.hblank_seq;
				gba_timing_schedule(gba, &gba->dma_event, 2);
				if (g_gba_mt_current && g_gba_mt_current->enabled
				    && lcd_y < GBA_LCD_H)
					gba_mt_ppu_hblank_snapshot(g_gba_mt_current, gba, lcd_y);
			}
			if (!hblank) {
				// Pipeline shift on hblank falling edge (HBLANK_END),
				// exactly one dot before lcd_x==0.  ST reads
				// dispcnt_pipeline[0] for the next line's affine update,
				// so the shift must complete before lcd_x==0.
				gba->ppu.dispcnt_pipeline[0] = gba->ppu.dispcnt_pipeline[1];
				gba->ppu.dispcnt_pipeline[1] = gba->ppu.dispcnt_pipeline[2];
				gba->ppu.dispcnt_pipeline[2] = gba_io_read16(gba, GBA_DISPCNT);
			}
		}

		if (vblank != gba->ppu.last_vblank) {
			gba->ppu.last_vblank = vblank;
			bool vblank_irq_en   = SB_BFE(gba_io_read16(gba, GBA_DISPSTAT), 3, 1);
			if (vblank) {
				if (vblank_irq_en)
					new_if |= (1 << GBA_INT_LCD_VBLANK);
				++gba->ppu.vblank_seq;
				gba_timing_schedule(gba, &gba->dma_event, 2);
				gba->frame_in_progress = false;
				if (g_gba_mt_current && g_gba_mt_current->enabled)
					gba_mt_ppu_vblank_entry(g_gba_mt_current, gba);
			}
		}

		bool vcount_irq_en = SB_BFE(gba_io_read16(gba, GBA_DISPSTAT), 5, 1);
		UINT32 vcount_cmp2 = gba_io_read16(gba, GBA_DISPSTAT) >> 8;
		if (vcount == (INT32)vcount_cmp2 && vcount_irq_en)
			new_if |= (1 << GBA_INT_LCD_VCOUNT);
	}

	if (new_if)
		gba_send_interrupt(gba, 3, new_if);

	// Affine BG increment / reload at lcd_x == 0.
	// Must run on both render=true and render=false frames so affine
	// reference points don't drift during frame-skip / fast-forward.
	// Logic is bit-exact with ST (gba_ppu_event):
	//   - bg_mode != 0 && lcd_y != 0 guard
	//   - per-BG mosaic enable bit (bgcnt bit 6)
	//   - mosaic jump uses (lcd_y % mos_y) == 0
	//   - wrote_bgx/y: if CPU wrote a new ref point this scanline, SKIP
	//     the b/d increment (ST clears the flag and does NOT add b/d
	//     after a write); reload overwrites below
	if (lcd_x == 0 && lcd_y < GBA_LCD_H) {
		UINT16 dispcnt = gba->ppu.dispcnt_pipeline[0];
		INT32  bg_mode = SB_BFE(dispcnt, 0, 3);

		if (bg_mode != 0 && lcd_y != 0) {
			for (INT32 aff = 0; aff < 2; ++aff) {
				bool bg_en = SB_BFE(dispcnt, 8 + aff + 2, 1);
				if (!bg_en) continue;

				INT32  pb = (INT16)gba_io_read16(gba, GBA_BG2PB + aff * 0x10);
				INT32  pd = (INT16)gba_io_read16(gba, GBA_BG2PD + aff * 0x10);
				UINT16 bgcnt = gba_io_read16(gba, GBA_BG2CNT + aff * 2);
				bool mosaic = SB_BFE(bgcnt, 6, 1);

				if (gba->ppu.aff[aff].wrote_bgx) {
					// CPU wrote a new reference point: skip the per-line
					// b/d increment; reload below overwrites render_bgx
					// with the freshly-written value.
					gba->ppu.aff[aff].wrote_bgx = false;
				} else if (mosaic) {
					UINT16 mos_reg = gba_io_read16(gba, GBA_MOSAIC);
					INT32  mos_y   = SB_BFE(mos_reg, 4, 4) + 1;
					if ((lcd_y % mos_y) == 0) {
						gba->ppu.aff[aff].render_bgx += pb * mos_y;
						gba->ppu.aff[aff].render_bgy += pd * mos_y;
					}
				} else {
					gba->ppu.aff[aff].render_bgx += pb;
					gba->ppu.aff[aff].render_bgy += pd;
				}
				if (gba->ppu.aff[aff].wrote_bgy)
					gba->ppu.aff[aff].wrote_bgy = false;
			}
		}

		// Reload from BG2X/BG2Y when written this scanline, or
		// unconditionally on line 0.
		for (INT32 aff = 0; aff < 2; ++aff) {
			if (gba->ppu.aff[aff].wrote_bgx || lcd_y == 0) {
				gba->ppu.aff[aff].render_bgx = gba_io_read32(gba, GBA_BG2X + aff * 0x10);
				gba->ppu.aff[aff].render_bgx = SB_BFE(gba->ppu.aff[aff].render_bgx, 0, 28);
				gba->ppu.aff[aff].render_bgx = ((INT32)(gba->ppu.aff[aff].render_bgx << 4)) >> 4;
				gba->ppu.aff[aff].wrote_bgx  = false;
			}
			if (gba->ppu.aff[aff].wrote_bgy || lcd_y == 0) {
				gba->ppu.aff[aff].render_bgy = gba_io_read32(gba, GBA_BG2Y + aff * 0x10);
				gba->ppu.aff[aff].render_bgy = SB_BFE(gba->ppu.aff[aff].render_bgy, 0, 28);
				gba->ppu.aff[aff].render_bgy = ((INT32)(gba->ppu.aff[aff].render_bgy << 4)) >> 4;
				gba->ppu.aff[aff].wrote_bgy  = false;
			}
		}

		if (g_gba_mt_current && g_gba_mt_current->enabled && lcd_y < GBA_LCD_H)
			gba_mt_ppu_line_start_snapshot(g_gba_mt_current, gba, lcd_y);
	}

	// NOTE: gba_ppu_render_pixel / render_objs / render_scanline are NOT
	// called here.  Pixel composition runs only in the worker.  Calling
	// them here would double the per-frame pixel work and halve FPS.

	// Reschedule next PPU event on the absolute scanline grid,
	// compensating for dispatch lateness (matches ST gba_ppu_event).
	gba_timing_deschedule(gba, &gba->ppu_event);
	gba_timing_schedule(gba, &gba->ppu_event,
	                    fast_forward_ticks + 1 - (INT32)cycles_late);
}

// ===========================================================================
// Platform back-end primitives
//
// The vblank_publish flow is platform-agnostic; only these 4 primitives
// differ between POSIX and Win32:
//   gba_mt_plat_trywait_done()  — non-blocking poll for worker completion
//   gba_mt_plat_wait_done()     — blocking wait for worker completion
//   gba_mt_plat_release_job()   — post job semaphore (wake worker)
//   gba_mt_plat_mb()            — full memory barrier (publish ordering)
// ===========================================================================

// ---- POSIX primitives -----------------------------------------------------
#if GBA_HAVE_PTHREAD

static inline bool gba_mt_plat_trywait_done(gba_mt_t* mt) {
	// rv MUST be initialised to -1 so the EINTR loop has deterministic
	// behaviour even if sem_trywait somehow returns without writing it.
	int rv = -1;
	do { rv = sem_trywait(mt->done_sem); } while (rv == -1 && errno == EINTR);
	return (rv == 0);
}

static inline void gba_mt_plat_wait_done(gba_mt_t* mt) {
	for (;;) { int rv = sem_wait(mt->done_sem); if (rv == 0 || errno != EINTR) break; }
}

static inline void gba_mt_plat_release_job(gba_mt_t* mt) {
	sem_post(mt->job_sem);
}

// Full memory barrier.  __sync_synchronize is a compiler barrier on x86
// (TSO makes a HW barrier a no-op at runtime) and emits a DMB ISH on
// ARM/AArch64.  Works on both GCC and Clang.
static inline void gba_mt_plat_mb(void) {
	__sync_synchronize();
}

// ---- Win32 primitives -----------------------------------------------------
#elif GBA_HAVE_WINTHREAD

static inline bool gba_mt_plat_trywait_done(gba_mt_t* mt) {
	return (WaitForSingleObject(mt->done_sem, 0) == WAIT_OBJECT_0);
}

static inline void gba_mt_plat_wait_done(gba_mt_t* mt) {
	WaitForSingleObject(mt->done_sem, INFINITE);
}

static inline void gba_mt_plat_release_job(gba_mt_t* mt) {
	ReleaseSemaphore(mt->job_sem, 1, NULL);
}

static inline void gba_mt_plat_mb(void) {
	MemoryBarrier();
}

#endif

// ===========================================================================
// Platform-agnostic vblank publish
//
// 1. Wait (blocking in normal play, non-blocking poll in fast-forward)
//    for the worker to finish the previous frame, if it was busy.
// 2. Promote the newly-finished backbuf to front_idx.
// 3. Copy the front frame to the presentation buffer (memset black
//    until the very first worker frame completes).
// 4. If rendering this frame and the worker is free and the snapshot is
//    complete, post a new render job; otherwise just reset the write slot.
// ===========================================================================
static void gba_mt_ppu_vblank_publish(gba_mt_t* mt, gba_t* gba,
                                      gba_scratch_t* scratch)
{
	if (!mt || !mt->enabled) {
		gba->framebuffer = scratch->framebuffer;
		return;
	}

	// Step 1: synchronise with the worker.
	if (mt->worker_busy) {
		bool finished;
		if (mt->fast_forward) {
			// Non-blocking poll — if worker hasn't finished, skip this
			// vblank and keep showing the last good frame.
			finished = gba_mt_plat_trywait_done(mt);
		} else {
			// Blocking wait — tight pipeline for lowest latency.
			gba_mt_plat_wait_done(mt);
			finished = true;
		}
		if (finished) {
			mt->worker_busy = false;
			mt->job_pending  = false;
		}
		// else: worker still busy → no new job this vblank.
	}
	if (!mt->worker_busy && mt->ready_back_idx >= 0)
		mt->front_idx = mt->ready_back_idx;

	// Step 2: present the front buffer.  The copy is ~150 KB, so only do
	// it when the front frame actually changed; skip frames would
	// otherwise repeat it for nothing.
	if (mt->have_first_frame) {
		if (mt->front_idx != mt->presented_front_idx) {
			memcpy(scratch->framebuffer, mt->backbuf[mt->front_idx],
			       sizeof(scratch->framebuffer));
			mt->presented_front_idx = mt->front_idx;
		}
	} else if (!mt->presented_black) {
		memset(scratch->framebuffer, 0, sizeof(scratch->framebuffer));
		mt->presented_black = true;
	}
	gba->framebuffer = scratch->framebuffer;

	// Step 3: post a new job if we are rendering this frame and the
	// worker is free to accept new work.
	if (mt->want_render_this_frame && !mt->worker_busy) {
		int w = mt->w_idx;
		gba_mt_snapshot_t* snap = &mt->snapshots[w];
		if (snap->vram_copied) {
			// vblank_publish is called only after wait_done returned
			// (or worker was already idle), so snapshot slot w — which
			// main thread has been filling this entire frame — is
			// guaranteed not to be in use by the worker.  The worker
			// will write its output into backbuf[w] (1:1 slot mapping).
			// Publish the job.
			mt->r_idx = w;
			mt->w_idx = (w + 1) % GBA_MT_N_BUFFERS;
			// All publish-visible state (r_idx, w_idx, have_first_frame,
			// snapshot contents, front_idx) must be set BEFORE the
			// release barrier below, so the worker sees a consistent
			// snapshot after sem_wait(job_sem) returns.
			mt->job_pending      = true;
			mt->worker_busy      = true;
			mt->have_first_frame = true;
			// Pre-seed the new write slot with VBlank-time defaults
			// for the next frame's capture.
			gba_mt_reset_slot(mt, mt->w_idx, gba);
			// Main → worker publish barrier.
			gba_mt_plat_mb();
			gba_mt_plat_release_job(mt);
			mt->frame_count++;
		} else {
			// Snapshot not complete (should be rare — vblank_entry sets
			// vram_copied at VBlank edge, before we get here).  Reset
			// the slot and wait for the next vblank.
			gba_mt_reset_slot(mt, w, gba);
		}
		mt->want_render_this_frame = false;
	}
}

// ===========================================================================
// POSIX back-end: thread creation / destruction / worker loop
// ===========================================================================
#if GBA_HAVE_PTHREAD

static void* gba_mt_worker_thread(void* arg)
{
	gba_mt_t* mt = (gba_mt_t*)arg;
	for (;;) {
		// Wait for the next job (EINTR-safe).
		for (;;) { int rv = sem_wait(mt->job_sem); if (rv == 0 || errno != EINTR) break; }
		if (mt->exiting) break;
		if (!mt->job_pending) continue;     // spurious wake-up

		int ridx = mt->r_idx;
		// Load front_idx AFTER acquiring job_sem: the main thread
		// wrote it before releasing the semaphore, and sem_wait's
		// acquire semantics guarantee this read sees the new value.
		int front = mt->front_idx;
		UINT32* ghost_src = (front >= 0 && mt->have_first_frame)
			? (UINT32*)mt->backbuf[front] : NULL;

		gba_mt_render_frame_into(&mt->snapshots[ridx],
		                         (UINT32*)mt->backbuf[ridx], ghost_src);

		// Worker → main thread publish barrier: ensure all backbuf
		// writes are visible before we set ready_back_idx and post
		// done_sem.
		gba_mt_plat_mb();
		mt->ready_back_idx = ridx;
		mt->worker_busy    = false;
		mt->job_pending    = false;
		sem_post(mt->done_sem);
	}
	return NULL;
}

static INT32 gba_mt_init(gba_mt_t* mt)
{
	memset(mt, 0, sizeof(*mt));
	mt->enabled              = false;
	mt->exiting              = false;
	mt->w_idx = mt->r_idx = mt->front_idx = 0;
	mt->ready_back_idx       = -1;
	mt->worker_busy          = false;
	mt->job_pending          = false;
	mt->have_first_frame     = false;
	mt->presented_front_idx  = -1;
	mt->presented_black      = false;
	mt->fast_forward         = false;
	mt->frame_count          = 0;
	mt->want_render_this_frame = false;
	mt->job_sem = mt->done_sem = NULL;

	// Named semaphores (macOS does not support process-shared unnamed
	// semaphores via sem_init; named ones work everywhere).  Encode the
	// control-block pointer into the name to guarantee uniqueness across
	// multiple concurrent GbaCore instances without needing a global
	// atomic counter (which itself would be a thread-initialisation
	// race on some platforms).
	// Name format: "/fbngba{j|d}_<pointer-hex>" — max ~28 chars on
	// 64-bit ("/fbngbaj_0x7f1234567890abcd"), well within 40-byte
	// buffers.
	snprintf(mt->job_sem_name,  sizeof(mt->job_sem_name),
	         "/fbngbaj_%p", (void*)mt);
	snprintf(mt->done_sem_name, sizeof(mt->done_sem_name),
	         "/fbngbad_%p", (void*)mt);
	sem_unlink(mt->job_sem_name);
	sem_unlink(mt->done_sem_name);
	mt->job_sem  = sem_open(mt->job_sem_name,  O_CREAT, 0644, 0);
	mt->done_sem = sem_open(mt->done_sem_name, O_CREAT, 0644, 0);
	sem_unlink(mt->job_sem_name);
	sem_unlink(mt->done_sem_name);
	// sem_open returns SEM_FAILED ((sem_t*)-1) on error, not NULL.
	if (mt->job_sem  == SEM_FAILED) mt->job_sem  = NULL;
	if (mt->done_sem == SEM_FAILED) mt->done_sem = NULL;
	if (!mt->job_sem || !mt->done_sem) {
		if (mt->job_sem)  { sem_close(mt->job_sem);  mt->job_sem  = NULL; }
		if (mt->done_sem) { sem_close(mt->done_sem); mt->done_sem = NULL; }
		return 1;
	}

	memset(mt->backbuf,   0, sizeof(mt->backbuf));
	memset(mt->snapshots, 0, sizeof(mt->snapshots));

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 512 * 1024);
	if (pthread_create(&mt->worker, &attr, gba_mt_worker_thread, mt) != 0) {
		pthread_attr_destroy(&attr);
		sem_close(mt->job_sem);
		sem_close(mt->done_sem);
		mt->job_sem = mt->done_sem = NULL;
		return 1;
	}
	pthread_attr_destroy(&attr);
#if defined(__linux__) && defined(__GLIBC__)
	pthread_setname_np(mt->worker, "GBA-PPU");
#endif
	mt->enabled = true;
	return 0;
}

static void gba_mt_exit(gba_mt_t* mt)
{
	if (!mt) return;
	if (mt->enabled) {
		mt->exiting = true;
		// Wake the worker (it may be blocked on job_sem).  After it
		// sees exiting==true it breaks out without touching any
		// snapshot or backbuf.
		sem_post(mt->job_sem);
		pthread_join(mt->worker, NULL);
		if (mt->job_sem)  { sem_close(mt->job_sem);  mt->job_sem  = NULL; }
		if (mt->done_sem) { sem_close(mt->done_sem); mt->done_sem = NULL; }
		mt->enabled = false;
	}
	memset(mt, 0, sizeof(*mt));
}

static void gba_mt_reset(gba_mt_t* mt)
{
	if (!mt || !mt->enabled) return;
	// If a render job is in flight (reset/loadstate/ROM change), wait
	// for the worker to finish before we nuke snapshots/backbufs.
	if (mt->worker_busy || mt->job_pending) {
		while (mt->worker_busy)
			gba_mt_plat_wait_done(mt);
		// Drain any stale completion tokens (defensive — should be
		// at most one, but the loop handles pathological cases).
		while (gba_mt_plat_trywait_done(mt)) {}
		mt->worker_busy = false;
		mt->job_pending = false;
	}
	memset(mt->backbuf,   0, sizeof(mt->backbuf));
	memset(mt->snapshots, 0, sizeof(mt->snapshots));
	mt->ready_back_idx       = -1;
	mt->w_idx = mt->r_idx = mt->front_idx = 0;
	mt->frame_count          = 0;
	mt->have_first_frame     = false;
	mt->presented_front_idx  = -1;
	mt->presented_black      = false;
	mt->fast_forward         = false;
	mt->want_render_this_frame = false;
}

// ===========================================================================
// Win32 back-end: thread creation / destruction / worker loop
// ===========================================================================
#elif GBA_HAVE_WINTHREAD

static DWORD WINAPI gba_mt_worker_thread_win32(LPVOID arg)
{
	gba_mt_t* mt = (gba_mt_t*)arg;
	for (;;) {
		WaitForSingleObject(mt->job_sem, INFINITE);
		if (mt->exiting) break;
		if (!mt->job_pending) continue;

		int ridx = mt->r_idx;
		int front = mt->front_idx;
		UINT32* ghost_src = (front >= 0 && mt->have_first_frame)
			? (UINT32*)mt->backbuf[front] : NULL;

		gba_mt_render_frame_into(&mt->snapshots[ridx],
		                         (UINT32*)mt->backbuf[ridx], ghost_src);

		gba_mt_plat_mb();
		mt->ready_back_idx = ridx;
		mt->worker_busy    = false;
		mt->job_pending    = false;
		ReleaseSemaphore(mt->done_sem, 1, NULL);
	}
	return 0;
}

#if defined(_MSC_VER)
// Classic MSVC debugger thread-naming trick (no-op outside a debugger).
// Uses __try/__except SEH, which is MSVC-specific.
static void gba_mt_set_thread_name_win32(DWORD tid, const char* name)
{
	typedef struct { DWORD dwType; LPCSTR szName; DWORD dwThreadID; DWORD dwFlags; } TNI;
	TNI info;
	info.dwType     = 0x1000;
	info.szName     = name;
	info.dwThreadID = tid;
	info.dwFlags    = 0;
	__try {
		RaiseException(0x406D1388, 0,
		               sizeof(info)/sizeof(ULONG_PTR), (const ULONG_PTR*)&info);
	} __except(EXCEPTION_EXECUTE_HANDLER) {}
}
#endif

static INT32 gba_mt_init(gba_mt_t* mt)
{
	memset(mt, 0, sizeof(*mt));
	mt->enabled              = false;
	mt->exiting              = false;
	mt->w_idx = mt->r_idx = mt->front_idx = 0;
	mt->ready_back_idx       = -1;
	mt->worker_busy          = false;
	mt->job_pending          = false;
	mt->have_first_frame     = false;
	mt->presented_front_idx  = -1;
	mt->presented_black      = false;
	mt->fast_forward         = false;
	mt->frame_count          = 0;
	mt->want_render_this_frame = false;
	mt->worker  = NULL;
	mt->job_sem = mt->done_sem = NULL;

	mt->job_sem  = CreateSemaphoreA(NULL, 0, GBA_MT_N_BUFFERS + 1, NULL);
	mt->done_sem = CreateSemaphoreA(NULL, 0, GBA_MT_N_BUFFERS + 1, NULL);
	if (!mt->job_sem || !mt->done_sem) {
		if (mt->job_sem)  { CloseHandle(mt->job_sem);  mt->job_sem  = NULL; }
		if (mt->done_sem) { CloseHandle(mt->done_sem); mt->done_sem = NULL; }
		return 1;
	}

	memset(mt->backbuf,   0, sizeof(mt->backbuf));
	memset(mt->snapshots, 0, sizeof(mt->snapshots));

	DWORD tid = 0;
	mt->worker = CreateThread(NULL, 512 * 1024,
	                          gba_mt_worker_thread_win32, mt, 0, &tid);
	if (!mt->worker) {
		CloseHandle(mt->job_sem);
		CloseHandle(mt->done_sem);
		mt->job_sem = mt->done_sem = NULL;
		return 1;
	}
	SetThreadPriority(mt->worker, THREAD_PRIORITY_NORMAL);
#if defined(_MSC_VER)
	gba_mt_set_thread_name_win32(tid, "GBA-PPU");
#endif
	mt->enabled = true;
	return 0;
}

static void gba_mt_exit(gba_mt_t* mt)
{
	if (!mt) return;
	if (mt->enabled) {
		mt->exiting = true;
		ReleaseSemaphore(mt->job_sem, 1, NULL);
		WaitForSingleObject(mt->worker, INFINITE);
		CloseHandle(mt->worker); mt->worker = NULL;
		if (mt->job_sem)  { CloseHandle(mt->job_sem);  mt->job_sem  = NULL; }
		if (mt->done_sem) { CloseHandle(mt->done_sem); mt->done_sem = NULL; }
		mt->enabled = false;
	}
	memset(mt, 0, sizeof(*mt));
}

static void gba_mt_reset(gba_mt_t* mt)
{
	if (!mt || !mt->enabled) return;
	if (mt->worker_busy || mt->job_pending) {
		while (mt->worker_busy)
			gba_mt_plat_wait_done(mt);
		while (gba_mt_plat_trywait_done(mt)) {}
		mt->worker_busy = false;
		mt->job_pending = false;
	}
	memset(mt->backbuf,   0, sizeof(mt->backbuf));
	memset(mt->snapshots, 0, sizeof(mt->snapshots));
	mt->ready_back_idx       = -1;
	mt->w_idx = mt->r_idx = mt->front_idx = 0;
	mt->frame_count          = 0;
	mt->have_first_frame     = false;
	mt->presented_front_idx  = -1;
	mt->presented_black      = false;
	mt->fast_forward         = false;
	mt->want_render_this_frame = false;
}

#endif // back-end selection (POSIX / Win32)

// ===========================================================================
// Single-threaded fallback stubs
// (when GBA_DISABLE_MT is defined, or no threading support detected)
// ===========================================================================
#else // !GBA_MT_ENABLED

// Minimal dummy control block.  `enabled` MUST be the first field so that
// gba.cpp's `if (mt && mt->enabled)` test works identically to the MT path.
// In ST mode enabled is always false (the core is memset to 0 at init and
// gba_mt_init() is a no-op that never sets it to true), so the ST PPU
// event callback is always selected.
typedef struct {
	bool enabled;
	int  dummy;
} gba_mt_t;

// Real extern pointer (NOT a macro) — gba.cpp assigns to this variable
// (`g_gba_mt_current = mt;` / `= NULL;`) at frame boundaries, so it must
// be a proper lvalue.  gba.cpp provides the single definition
// `gba_mt_t* g_gba_mt_current = NULL;`, which works in both modes because
// the gba_mt_t type exists in both paths.
extern gba_mt_t* g_gba_mt_current;

// All gba_mt_* API functions are no-op inlines.  Their MT-enabled
// counterparts are defined (non-inline) in the POSIX/Win32 back-end
// sections above; these stubs let gba.cpp call the API unconditionally
// without #if guards.
static inline INT32 gba_mt_init(gba_mt_t* mt)                                 { (void)mt; return 1; }
static inline void  gba_mt_exit(gba_mt_t* mt)                                 { (void)mt; }
static inline void  gba_mt_reset(gba_mt_t* mt)                                { (void)mt; }
static inline void  gba_mt_begin_frame(gba_mt_t* mt, gba_t* g, sb_emu_state_t* e)
                                                                              { (void)mt; (void)g; (void)e; }
static inline void  gba_mt_ppu_hblank_snapshot(gba_mt_t* mt, gba_t* g, INT32 y)
                                                                              { (void)mt; (void)g; (void)y; }
static inline void  gba_mt_ppu_line_start_snapshot(gba_mt_t* mt, gba_t* g, INT32 y)
                                                                              { (void)mt; (void)g; (void)y; }
static inline void  gba_mt_ppu_vblank_entry(gba_mt_t* mt, gba_t* g)           { (void)mt; (void)g; }
static inline void  gba_mt_ppu_vblank_publish(gba_mt_t* mt, gba_t* g, gba_scratch_t* s)
                                                                              { (void)mt; (void)g; (void)s; }
static inline bool  gba_mt_enabled(const gba_mt_t* mt)                        { (void)mt; return false; }

// gba_ppu_event_mt stub: even though the `if (mt && mt->enabled)` branch
// in gba_tick() is always false in ST mode (enabled stays 0 forever),
// C++ requires the function name to be declared at the point where its
// address is taken.  The stub delegates straight to the original ST PPU
// event so that if it is ever accidentally called, behaviour is correct
// rather than undefined.
static inline void gba_ppu_event_mt(gba_t* gba, sb_emu_state_t* emu, UINT32 cycles_late)
{
	gba_ppu_event(gba, emu, cycles_late);
}

#endif // GBA_MT_ENABLED

#ifdef __cplusplus
}
#endif

#endif // GBA_PPU_H
