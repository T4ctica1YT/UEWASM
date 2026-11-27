#pragma once
#include "dxbc.h"

/* constant layout shared by the translator (d3d9_wgsl.c) and the device (d3d9.c), in vec4 units */
#define D3D9_CB_INT 256     /* i0-i15, as int bits */
#define D3D9_CB_BOOL 272    /* b0-b15, .x nonzero = true */
#define D3D9_CB_EXTRA 288   /* vs: .xy half-pixel offset; ps: .x alpha test D3DCMPFUNC (8 = off), .y reference (0-1) */
#define D3D9_CB_PALETTE 296 /* fixed-function vertex blending: 96 world matrices of 3 rows (see d3d9_ffp.h) */
#define D3D9_CB_VEC4S 584

/* vertex shader trailer after END: this, then a mask of unsigned integer input registers and one of signed ones */
#define D3D9_INT_MAGIC 0x39494D55u

int d3d9_to_wgsl(const uint8_t* code, size_t size, const DxWgslOpts* opt, DxWgsl* out);
int d3d9_reflect(const uint8_t* code, size_t size, DxWgsl* out);
/* usage (0xFF = undeclared) and usage index of each vertex shader input register */
int d3d9_vs_inputs(const uint8_t* code, size_t size, uint8_t usage[16], uint8_t index[16]);
