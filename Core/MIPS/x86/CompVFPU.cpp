// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <cmath>
#include <limits>
#include <xmmintrin.h>

#include "base/logging.h"
#include "math/math_util.h"

#include "Common/CPUDetect.h"
#include "Core/MemMap.h"
#include "Core/Config.h"
#include "Core/Reporting.h"
#include "Core/MIPS/MIPSAnalyst.h"
#include "Core/MIPS/MIPSCodeUtils.h"
#include "Core/MIPS/MIPSVFPUUtils.h"
#include "Core/MIPS/x86/Jit.h"
#include "Core/MIPS/x86/RegCache.h"

// All functions should have CONDITIONAL_DISABLE, so we can narrow things down to a file quickly.
// Currently known non working ones should have DISABLE.

// #define CONDITIONAL_DISABLE { fpr.ReleaseSpillLocks(); Comp_Generic(op); return; }
#define CONDITIONAL_DISABLE ;
#define DISABLE { fpr.ReleaseSpillLocks(); Comp_Generic(op); return; }

#define _RS MIPS_GET_RS(op)
#define _RT MIPS_GET_RT(op)
#define _RD MIPS_GET_RD(op)
#define _FS MIPS_GET_FS(op)
#define _FT MIPS_GET_FT(op)
#define _FD MIPS_GET_FD(op)
#define _SA MIPS_GET_SA(op)
#define _POS  ((op>> 6) & 0x1F)
#define _SIZE ((op>>11) & 0x1F)
#define _IMM16 (signed short)(op & 0xFFFF)
#define _IMM26 (op & 0x03FFFFFF)

namespace MIPSComp
{
using namespace Gen;
using namespace X64JitConstants;

static const float one = 1.0f;
static const float minus_one = -1.0f;
static const float zero = 0.0f;

const u32 MEMORY_ALIGNED16( noSignMask[4] ) = {0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF};
const u32 MEMORY_ALIGNED16( signBitAll[4] ) = {0x80000000, 0x80000000, 0x80000000, 0x80000000};
const u32 MEMORY_ALIGNED16( signBitLower[4] ) = {0x80000000, 0, 0, 0};
const float MEMORY_ALIGNED16( oneOneOneOne[4] ) = {1.0f, 1.0f, 1.0f, 1.0f};
const u32 MEMORY_ALIGNED16( solidOnes[4] ) = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
const u32 MEMORY_ALIGNED16( lowOnes[4] ) = {0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000};
const u32 MEMORY_ALIGNED16( lowZeroes[4] ) = {0x00000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
const u32 MEMORY_ALIGNED16( fourinfnan[4] ) = {0x7F800000, 0x7F800000, 0x7F800000, 0x7F800000};
const float MEMORY_ALIGNED16( identityMatrix[4][4]) = { { 1.0f, 0, 0, 0 }, { 0, 1.0f, 0, 0 }, { 0, 0, 1.0f, 0 }, { 0, 0, 0, 1.0f} };

void Jit::Comp_VPFX(MIPSOpcode op)
{
	CONDITIONAL_DISABLE;
	int data = op & 0xFFFFF;
	int regnum = (op >> 24) & 3;
	switch (regnum) {
	case 0:  // S
		js.prefixS = data;
		js.prefixSFlag = JitState::PREFIX_KNOWN_DIRTY;
		break;
	case 1:  // T
		js.prefixT = data;
		js.prefixTFlag = JitState::PREFIX_KNOWN_DIRTY;
		break;
	case 2:  // D
		js.prefixD = data;
		js.prefixDFlag = JitState::PREFIX_KNOWN_DIRTY;
		break;
	}
}

void Jit::ApplyPrefixST(u8 *vregs, u32 prefix, VectorSize sz) {
	if (prefix == 0xE4) return;

	int n = GetNumVectorElements(sz);
	u8 origV[4];
	static const float constantArray[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f/3.f, 0.25f, 1.f/6.f};

	for (int i = 0; i < n; i++)
		origV[i] = vregs[i];

	for (int i = 0; i < n; i++)
	{
		int regnum = (prefix >> (i*2)) & 3;
		int abs    = (prefix >> (8+i)) & 1;
		int negate = (prefix >> (16+i)) & 1;
		int constants = (prefix >> (12+i)) & 1;

		// Unchanged, hurray.
		if (!constants && regnum == i && !abs && !negate)
			continue;

		// This puts the value into a temp reg, so we won't write the modified value back.
		vregs[i] = fpr.GetTempV();
		fpr.MapRegV(vregs[i], MAP_NOINIT | MAP_DIRTY);

		if (!constants) {
			// Prefix may say "z, z, z, z" but if this is a pair, we force to x.
			// TODO: But some ops seem to use const 0 instead?
			if (regnum >= n) {
				ERROR_LOG_REPORT(CPU, "Invalid VFPU swizzle: %08x / %d", prefix, sz);
				regnum = 0;
			}
			fpr.SimpleRegV(origV[regnum], 0);
			MOVSS(fpr.VX(vregs[i]), fpr.V(origV[regnum]));
			if (abs) {
				ANDPS(fpr.VX(vregs[i]), M(&noSignMask));
			}
		} else {
			MOVSS(fpr.VX(vregs[i]), M(&constantArray[regnum + (abs<<2)]));
		}

		if (negate)
			XORPS(fpr.VX(vregs[i]), M(&signBitLower));

		// TODO: This probably means it will swap out soon, inefficiently...
		fpr.ReleaseSpillLockV(vregs[i]);
	}
}

void Jit::GetVectorRegsPrefixD(u8 *regs, VectorSize sz, int vectorReg) {
	_assert_(js.prefixDFlag & JitState::PREFIX_KNOWN);

	GetVectorRegs(regs, sz, vectorReg);
	if (js.prefixD == 0)
		return;

	int n = GetNumVectorElements(sz);
	for (int i = 0; i < n; i++)
	{
		// Hopefully this is rare, we'll just write it into a reg we drop.
		if (js.VfpuWriteMask(i))
			regs[i] = fpr.GetTempV();
	}
}

void Jit::ApplyPrefixD(const u8 *vregs, VectorSize sz) {
	_assert_(js.prefixDFlag & JitState::PREFIX_KNOWN);
	if (!js.prefixD) return;

	int n = GetNumVectorElements(sz);
	for (int i = 0; i < n; i++)
	{
		if (js.VfpuWriteMask(i))
			continue;

		int sat = (js.prefixD >> (i * 2)) & 3;
		if (sat == 1)
		{
			fpr.MapRegV(vregs[i], MAP_DIRTY);

			// Zero out XMM0 if it was <= +0.0f (but skip NAN.)
			MOVSS(R(XMM0), fpr.VX(vregs[i]));
			CMPLESS(XMM0, M(&zero));
			ANDNPS(XMM0, fpr.V(vregs[i]));

			// Retain a NAN in XMM0 (must be second operand.)
			MOVSS(fpr.VX(vregs[i]), M(&one));
			MINSS(fpr.VX(vregs[i]), R(XMM0));
		}
		else if (sat == 3)
		{
			fpr.MapRegV(vregs[i], MAP_DIRTY);

			// Check for < -1.0f, but careful of NANs.
			MOVSS(XMM1, M(&minus_one));
			MOVSS(R(XMM0), fpr.VX(vregs[i]));
			CMPLESS(XMM0, R(XMM1));
			// If it was NOT less, the three ops below do nothing.
			// Otherwise, they replace the value with -1.0f.
			ANDPS(XMM1, R(XMM0));
			ANDNPS(XMM0, fpr.V(vregs[i]));
			ORPS(XMM0, R(XMM1));

			// Retain a NAN in XMM0 (must be second operand.)
			MOVSS(fpr.VX(vregs[i]), M(&one));
			MINSS(fpr.VX(vregs[i]), R(XMM0));
		}
	}
}

// Vector regs can overlap in all sorts of swizzled ways.
// This does allow a single overlap in sregs[i].
bool IsOverlapSafeAllowS(int dreg, int di, int sn, u8 sregs[], int tn = 0, u8 tregs[] = NULL)
{
	for (int i = 0; i < sn; ++i)
	{
		if (sregs[i] == dreg && i != di)
			return false;
	}
	for (int i = 0; i < tn; ++i)
	{
		if (tregs[i] == dreg)
			return false;
	}

	// Hurray, no overlap, we can write directly.
	return true;
}

bool IsOverlapSafe(int dreg, int di, int sn, u8 sregs[], int tn = 0, u8 tregs[] = NULL)
{
	return IsOverlapSafeAllowS(dreg, di, sn, sregs, tn, tregs) && sregs[di] != dreg;
}

static u32 MEMORY_ALIGNED16(ssLoadStoreTemp);

void Jit::Comp_SV(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	s32 imm = (signed short)(op&0xFFFC);
	int vt = ((op >> 16) & 0x1f) | ((op & 3) << 5);
	MIPSGPReg rs = _RS;

	switch (op >> 26)
	{
	case 50: //lv.s  // VI(vt) = Memory::Read_U32(addr);
		{
			gpr.Lock(rs);
			gpr.MapReg(rs, true, false);
			fpr.MapRegV(vt, MAP_DIRTY | MAP_NOINIT);

			JitSafeMem safe(this, rs, imm);
			safe.SetFar();
			OpArg src;
			if (safe.PrepareRead(src, 4))
			{
				MOVSS(fpr.VX(vt), safe.NextFastAddress(0));
			}
			if (safe.PrepareSlowRead(safeMemFuncs.readU32))
			{
				MOVD_xmm(fpr.VX(vt), R(EAX));
			}
			safe.Finish();

			gpr.UnlockAll();
			fpr.ReleaseSpillLocks();
		}
		break;

	case 58: //sv.s   // Memory::Write_U32(VI(vt), addr);
		{
			gpr.Lock(rs);
			gpr.MapReg(rs, true, false);

			// Even if we don't use real SIMD there's still 8 or 16 scalar float registers.
			fpr.MapRegV(vt, 0);

			JitSafeMem safe(this, rs, imm);
			safe.SetFar();
			OpArg dest;
			if (safe.PrepareWrite(dest, 4))
			{
				MOVSS(safe.NextFastAddress(0), fpr.VX(vt));
			}
			if (safe.PrepareSlowWrite())
			{
				MOVSS(M(&ssLoadStoreTemp), fpr.VX(vt));
				safe.DoSlowWrite(safeMemFuncs.writeU32, M(&ssLoadStoreTemp), 0);
			}
			safe.Finish();

			fpr.ReleaseSpillLocks();
			gpr.UnlockAll();
		}
		break;

	default:
		DISABLE;
	}
}

void Jit::Comp_SVQ(MIPSOpcode op)
{
	CONDITIONAL_DISABLE;

	int imm = (signed short)(op&0xFFFC);
	int vt = (((op >> 16) & 0x1f)) | ((op&1) << 5);
	MIPSGPReg rs = _RS;

	switch (op >> 26)
	{
	case 53: //lvl.q/lvr.q
		{
			if (!g_Config.bFastMemory) {
				DISABLE;
			}
			DISABLE;

			gpr.MapReg(rs, true, false);
			gpr.FlushLockX(ECX);
			u8 vregs[4];
			GetVectorRegs(vregs, V_Quad, vt);
			MOV(32, R(EAX), gpr.R(rs));
			ADD(32, R(EAX), Imm32(imm));
#ifdef _M_IX86
			AND(32, R(EAX), Imm32(Memory::MEMVIEW32_MASK));
#endif
			MOV(32, R(ECX), R(EAX));
			SHR(32, R(EAX), Imm8(2));
			AND(32, R(EAX), Imm32(0x3));
			CMP(32, R(EAX), Imm32(0));
			FixupBranch next = J_CC(CC_NE);

			auto PSPMemAddr = [](X64Reg scaled, int offset) {
#ifdef _M_IX86
				return MDisp(scaled, (u32)Memory::base + offset);
#else
				return MComplex(MEMBASEREG, scaled, 1, offset);
#endif
			};

			fpr.MapRegsV(vregs, V_Quad, MAP_DIRTY);

			// Offset = 0
			MOVSS(fpr.RX(vregs[3]), PSPMemAddr(EAX, 0));

			FixupBranch skip0 = J();
			SetJumpTarget(next);
			CMP(32, R(EAX), Imm32(1));
			next = J_CC(CC_NE);

			// Offset = 1
			MOVSS(fpr.RX(vregs[3]), PSPMemAddr(EAX, 4));
			MOVSS(fpr.RX(vregs[2]), PSPMemAddr(EAX, 0));

			FixupBranch skip1 = J();
			SetJumpTarget(next);
			CMP(32, R(EAX), Imm32(2));
			next = J_CC(CC_NE);

			// Offset = 2
			MOVSS(fpr.RX(vregs[3]), PSPMemAddr(EAX, 8));
			MOVSS(fpr.RX(vregs[2]), PSPMemAddr(EAX, 4));
			MOVSS(fpr.RX(vregs[1]), PSPMemAddr(EAX, 0));

			FixupBranch skip2 = J();
			SetJumpTarget(next);
			CMP(32, R(EAX), Imm32(3));
			next = J_CC(CC_NE);

			// Offset = 3
			MOVSS(fpr.RX(vregs[3]), PSPMemAddr(EAX, 12));
			MOVSS(fpr.RX(vregs[2]), PSPMemAddr(EAX, 8));
			MOVSS(fpr.RX(vregs[1]), PSPMemAddr(EAX, 4));
			MOVSS(fpr.RX(vregs[0]), PSPMemAddr(EAX, 0));

			SetJumpTarget(next);
			SetJumpTarget(skip0);
			SetJumpTarget(skip1);
			SetJumpTarget(skip2);

			gpr.UnlockAll();
			fpr.ReleaseSpillLocks();
		}
		break;

	case 54: //lv.q
		{
			gpr.Lock(rs);
			gpr.MapReg(rs, true, false);
	
			u8 vregs[4];
			GetVectorRegs(vregs, V_Quad, vt);

			if (g_Config.bFastMemory && fpr.TryMapRegsVS(vregs, V_Quad, MAP_NOINIT | MAP_DIRTY)) {
				JitSafeMem safe(this, rs, imm);
				safe.SetFar();
				OpArg src;
				if (safe.PrepareRead(src, 16)) {
					MOVAPS(fpr.VSX(vregs), safe.NextFastAddress(0));
				} else {
					// Hmm... probably never happens.
				}
				safe.Finish();
				gpr.UnlockAll();
				fpr.ReleaseSpillLocks();
				return;
			}

			fpr.MapRegsV(vregs, V_Quad, MAP_DIRTY | MAP_NOINIT);

			JitSafeMem safe(this, rs, imm);
			safe.SetFar();
			OpArg src;
			if (safe.PrepareRead(src, 16))
			{
				// Just copy 4 words the easiest way while not wasting registers.
				for (int i = 0; i < 4; i++)
					MOVSS(fpr.VX(vregs[i]), safe.NextFastAddress(i * 4));
			}
			if (safe.PrepareSlowRead(safeMemFuncs.readU32))
			{
				for (int i = 0; i < 4; i++)
				{
					safe.NextSlowRead(safeMemFuncs.readU32, i * 4);
					MOVD_xmm(fpr.VX(vregs[i]), R(EAX));
				}
			}
			safe.Finish();

			gpr.UnlockAll();
			fpr.ReleaseSpillLocks();
		}
		break;

	case 62: //sv.q
		{
			gpr.Lock(rs);
			gpr.MapReg(rs, true, false);

			u8 vregs[4];
			GetVectorRegs(vregs, V_Quad, vt);

			if (g_Config.bFastMemory && fpr.TryMapRegsVS(vregs, V_Quad, 0)) {
				JitSafeMem safe(this, rs, imm);
				safe.SetFar();
				OpArg dest;
				if (safe.PrepareWrite(dest, 16)) {
					MOVAPS(safe.NextFastAddress(0), fpr.VSX(vregs));
				} else {
					// Hmm... probably never happens.
				}
				safe.Finish();
				gpr.UnlockAll();
				fpr.ReleaseSpillLocks();
				return;
			}

			// Even if we don't use real SIMD there's still 8 or 16 scalar float registers.
			fpr.MapRegsV(vregs, V_Quad, 0);

			JitSafeMem safe(this, rs, imm);
			safe.SetFar();
			OpArg dest;
			if (safe.PrepareWrite(dest, 16))
			{
				for (int i = 0; i < 4; i++)
					MOVSS(safe.NextFastAddress(i * 4), fpr.VX(vregs[i]));
			}
			if (safe.PrepareSlowWrite())
			{
				for (int i = 0; i < 4; i++)
				{
					MOVSS(M(&ssLoadStoreTemp), fpr.VX(vregs[i]));
					safe.DoSlowWrite(safeMemFuncs.writeU32, M(&ssLoadStoreTemp), i * 4);
				}
			}
			safe.Finish();

			gpr.UnlockAll();
			fpr.ReleaseSpillLocks();
		}
		break;

	default:
		DISABLE;
		break;
	}
}

void Jit::Comp_VVectorInit(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int type = (op >> 16) & 0xF;
	u8 dregs[4];
	GetVectorRegsPrefixD(dregs, sz, _VD);

	if (fpr.TryMapRegsVS(dregs, sz, MAP_NOINIT | MAP_DIRTY)) {
		if (type == 6) {
			XORPS(fpr.VSX(dregs), fpr.VS(dregs));
		} else if (type == 7) {
			MOVAPS(fpr.VSX(dregs), M(&oneOneOneOne));
		} else {
			DISABLE;
		}
		ApplyPrefixD(dregs, sz);
		fpr.ReleaseSpillLocks();
		return;
	}

	switch (type) {
	case 6: // v=zeros; break;  //vzero
		XORPS(XMM0, R(XMM0));
		break;
	case 7: // v=ones; break;   //vone
		MOVSS(XMM0, M(&one));
		break;
	default:
		DISABLE;
		break;
	}

	int n = GetNumVectorElements(sz);
	fpr.MapRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);
	for (int i = 0; i < n; ++i)
		MOVSS(fpr.VX(dregs[i]), R(XMM0));
	ApplyPrefixD(dregs, sz);

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_VIdt(MIPSOpcode op) {
	CONDITIONAL_DISABLE;
	if (js.HasUnknownPrefix())
		DISABLE;

	int vd = _VD;
	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 dregs[4];
	GetVectorRegsPrefixD(dregs, sz, _VD);
	if (fpr.TryMapRegsVS(dregs, sz, MAP_NOINIT | MAP_DIRTY)) {
		int row = vd & (n - 1);
		MOVAPS(fpr.VSX(dregs), M(identityMatrix[row]));
		ApplyPrefixD(dregs, sz);
		fpr.ReleaseSpillLocks();
		return;
	}

	XORPS(XMM0, R(XMM0));
	MOVSS(XMM1, M(&one));
	fpr.MapRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);
	switch (sz)
	{
	case V_Pair:
		MOVSS(fpr.VX(dregs[0]), R((vd&1)==0 ? XMM1 : XMM0));
		MOVSS(fpr.VX(dregs[1]), R((vd&1)==1 ? XMM1 : XMM0));
		break;
	case V_Quad:
		MOVSS(fpr.VX(dregs[0]), R((vd&3)==0 ? XMM1 : XMM0));
		MOVSS(fpr.VX(dregs[1]), R((vd&3)==1 ? XMM1 : XMM0));
		MOVSS(fpr.VX(dregs[2]), R((vd&3)==2 ? XMM1 : XMM0));
		MOVSS(fpr.VX(dregs[3]), R((vd&3)==3 ? XMM1 : XMM0));
		break;
	default:
		_dbg_assert_msg_(CPU,0,"Trying to interpret instruction that can't be interpreted");
		break;
	}
	ApplyPrefixD(dregs, sz);
	fpr.ReleaseSpillLocks();
}

void Jit::Comp_VDot(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	// TODO: Force read one of them into regs? probably not.
	u8 sregs[4], tregs[4], dregs[1];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixT(tregs, sz, _VT);
	GetVectorRegsPrefixD(dregs, V_Single, _VD);

	// With SSE2, these won't really give any performance benefit on their own, but may reduce
	// conversion costs from/to SIMD form. However, the SSE4.1 DPPS may be worth it.
	// Benchmarking will have to decide whether to enable this on < SSE4.1. Also a HADDPS version
	// for SSE3 could be written.
	if (fpr.TryMapDirtyInInVS(dregs, V_Single, sregs, sz, tregs, sz)) {
		switch (sz) {
		case V_Pair:
			if (cpu_info.bSSE4_1) {
				if (fpr.VSX(dregs) != fpr.VSX(sregs) && fpr.VSX(dregs) != fpr.VSX(tregs)) {
					MOVAPS(fpr.VSX(dregs), fpr.VS(sregs));
					DPPS(fpr.VSX(dregs), fpr.VS(tregs), 0x31);
				} else {
					MOVAPS(XMM0, fpr.VS(sregs));
					DPPS(XMM0, fpr.VS(tregs), 0x31);
					MOVAPS(fpr.VSX(dregs), R(XMM0));
				}
			} else {
				MOVAPS(XMM0, fpr.VS(sregs));
				MULPS(XMM0, fpr.VS(tregs));
				MOVAPS(R(XMM1), XMM0);
				SHUFPS(XMM1, R(XMM0), _MM_SHUFFLE(1, 1, 1, 1));
				ADDPS(XMM1, R(XMM0));
				MOVAPS(fpr.VS(dregs), XMM1);
			}
			break;
		case V_Triple:
			if (cpu_info.bSSE4_1) {
				if (fpr.VSX(dregs) != fpr.VSX(sregs) && fpr.VSX(dregs) != fpr.VSX(tregs)) {
					MOVAPS(fpr.VSX(dregs), fpr.VS(sregs));
					DPPS(fpr.VSX(dregs), fpr.VS(tregs), 0x71);
				} else {
					MOVAPS(XMM0, fpr.VS(sregs));
					DPPS(XMM0, fpr.VS(tregs), 0x71);
					MOVAPS(fpr.VSX(dregs), R(XMM0));
				}
			} else {
				MOVAPS(XMM0, fpr.VS(sregs));
				MULPS(XMM0, fpr.VS(tregs));
				MOVAPS(R(XMM1), XMM0);
				SHUFPS(XMM1, R(XMM0), _MM_SHUFFLE(3, 2, 1, 1));
				ADDSS(XMM1, R(XMM0));
				SHUFPS(XMM0, R(XMM1), _MM_SHUFFLE(3, 2, 2, 2));
				ADDSS(XMM1, R(XMM0));
				MOVAPS(fpr.VS(dregs), XMM1);
			}
			break;
		case V_Quad:
			if (cpu_info.bSSE4_1) {
				if (fpr.VSX(dregs) != fpr.VSX(sregs) && fpr.VSX(dregs) != fpr.VSX(tregs)) {
					MOVAPS(fpr.VSX(dregs), fpr.VS(sregs));
					DPPS(fpr.VSX(dregs), fpr.VS(tregs), 0xF1);
				} else {
					MOVAPS(XMM0, fpr.VS(sregs));
					DPPS(XMM0, fpr.VS(tregs), 0xF1);
					MOVAPS(fpr.VSX(dregs), R(XMM0));
				}
			} /* else if (cpu_info.bSSE3) {   // This is slower than the SSE2 solution on my Ivy!
				MOVAPS(XMM0, fpr.VS(sregs));
				MOVAPS(XMM1, fpr.VS(tregs));
				HADDPS(XMM0, R(XMM1));
				HADDPS(XMM0, R(XMM0));
				MOVAPS(fpr.VSX(dregs), R(XMM0));
			} */ else {
				MOVAPS(XMM0, fpr.VS(sregs));
				MOVAPS(XMM1, fpr.VS(tregs));
				MULPS(XMM0, R(XMM1));
				MOVAPS(XMM1, R(XMM0));
				SHUFPS(XMM1, R(XMM1), _MM_SHUFFLE(2, 3, 0, 1));
				ADDPS(XMM0, R(XMM1));
				MOVAPS(XMM1, R(XMM0));
				SHUFPS(XMM1, R(XMM1), _MM_SHUFFLE(0, 1, 2, 3));
				ADDSS(XMM0, R(XMM1));
				MOVAPS(fpr.VSX(dregs), R(XMM0));
			}
			break;
		default:
			DISABLE;
		}
		ApplyPrefixD(dregs, V_Single);
		fpr.ReleaseSpillLocks();
		return;
	}

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(tregs, sz, 0);
	fpr.SimpleRegsV(dregs, V_Single, MAP_DIRTY | MAP_NOINIT);

	X64Reg tempxreg = XMM0;
	if (IsOverlapSafe(dregs[0], 0, n, sregs, n, tregs)) {
		fpr.MapRegsV(dregs, V_Single, MAP_DIRTY | MAP_NOINIT);
		tempxreg = fpr.VX(dregs[0]);
	}

	// Need to start with +0.0f so it doesn't result in -0.0f.
	MOVSS(tempxreg, fpr.V(sregs[0]));
	MULSS(tempxreg, fpr.V(tregs[0]));
	for (int i = 1; i < n; i++)
	{
		// sum += s[i]*t[i];
		MOVSS(XMM1, fpr.V(sregs[i]));
		MULSS(XMM1, fpr.V(tregs[i]));
		ADDSS(tempxreg, R(XMM1));
	}

	if (!fpr.V(dregs[0]).IsSimpleReg(tempxreg)) {
		fpr.MapRegsV(dregs, V_Single, MAP_DIRTY | MAP_NOINIT);
		MOVSS(fpr.V(dregs[0]), tempxreg);
	}

	ApplyPrefixD(dregs, V_Single);

	fpr.ReleaseSpillLocks();
}


void Jit::Comp_VHdp(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 sregs[4], tregs[4], dregs[1];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixT(tregs, sz, _VT);
	GetVectorRegsPrefixD(dregs, V_Single, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(tregs, sz, 0);
	fpr.SimpleRegsV(dregs, V_Single, MAP_DIRTY | MAP_NOINIT);

	X64Reg tempxreg = XMM0;
	if (IsOverlapSafe(dregs[0], 0, n, sregs, n, tregs))
	{
		fpr.MapRegsV(dregs, V_Single, MAP_DIRTY | MAP_NOINIT);
		tempxreg = fpr.VX(dregs[0]);
	}

	// Need to start with +0.0f so it doesn't result in -0.0f.
	MOVSS(tempxreg, fpr.V(sregs[0]));
	MULSS(tempxreg, fpr.V(tregs[0]));
	for (int i = 1; i < n; i++)
	{
		// sum += (i == n-1) ? t[i] : s[i]*t[i];
		if (i == n - 1) {
			ADDSS(tempxreg, fpr.V(tregs[i]));
		} else {
			MOVSS(XMM1, fpr.V(sregs[i]));
			MULSS(XMM1, fpr.V(tregs[i]));
			ADDSS(tempxreg, R(XMM1));
		}
	}

	if (!fpr.V(dregs[0]).IsSimpleReg(tempxreg)) {
		fpr.MapRegsV(dregs, V_Single, MAP_DIRTY | MAP_NOINIT);
		MOVSS(fpr.V(dregs[0]), tempxreg);
	}

	ApplyPrefixD(dregs, V_Single);

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_VCrossQuat(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 sregs[4], tregs[4], dregs[4];
	GetVectorRegs(sregs, sz, _VS);
	GetVectorRegs(tregs, sz, _VT);
	GetVectorRegs(dregs, sz, _VD);

	if (sz == V_Triple) {
		// Cross product vcrsp.t
		if (fpr.TryMapDirtyInInVS(dregs, sz, sregs, sz, tregs, sz)) {
			MOVAPS(XMM0, fpr.VS(tregs));
			MOVAPS(XMM1, fpr.VS(sregs));
			SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(3, 0, 2, 1));
			SHUFPS(XMM1, R(XMM1), _MM_SHUFFLE(3, 0, 2, 1));
			MULPS(XMM0, fpr.VS(sregs));
			MULPS(XMM1, fpr.VS(tregs));
			SUBPS(XMM0, R(XMM1));
			SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(3, 0, 2, 1));
			MOVAPS(fpr.VS(dregs), XMM0);
			fpr.ReleaseSpillLocks();
			return;
		}

		// Flush SIMD.
		fpr.SimpleRegsV(sregs, sz, 0);
		fpr.SimpleRegsV(tregs, sz, 0);
		fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

		fpr.MapRegsV(sregs, sz, 0);
	
		// Compute X
		MOVSS(XMM0, fpr.V(sregs[1]));
		MULSS(XMM0, fpr.V(tregs[2]));
		MOVSS(XMM1, fpr.V(sregs[2]));
		MULSS(XMM1, fpr.V(tregs[1]));
		SUBSS(XMM0, R(XMM1));
		MOVSS(fpr.V(dregs[0]), XMM0);

		// Compute Y
		MOVSS(XMM0, fpr.V(sregs[2]));
		MULSS(XMM0, fpr.V(tregs[0]));
		MOVSS(XMM1, fpr.V(sregs[0]));
		MULSS(XMM1, fpr.V(tregs[2]));
		SUBSS(XMM0, R(XMM1));
		MOVSS(fpr.V(dregs[1]), XMM0);

		// Compute Z
		MOVSS(XMM0, fpr.V(sregs[0]));
		MULSS(XMM0, fpr.V(tregs[1]));
		MOVSS(XMM1, fpr.V(sregs[1]));
		MULSS(XMM1, fpr.V(tregs[0]));
		SUBSS(XMM0, R(XMM1));
		MOVSS(fpr.V(dregs[2]), XMM0);
	} else if (sz == V_Quad) {
		// Flush SIMD.
		fpr.SimpleRegsV(sregs, sz, 0);
		fpr.SimpleRegsV(tregs, sz, 0);
		fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

		// Quaternion product  vqmul.q
		fpr.MapRegsV(sregs, sz, 0);

		// Compute X
		// d[0] = s[0] * t[3] + s[1] * t[2] - s[2] * t[1] + s[3] * t[0];
		MOVSS(XMM0, fpr.V(sregs[0]));
		MULSS(XMM0, fpr.V(tregs[3]));
		MOVSS(XMM1, fpr.V(sregs[1]));
		MULSS(XMM1, fpr.V(tregs[2]));
		ADDSS(XMM0, R(XMM1));
		MOVSS(XMM1, fpr.V(sregs[2]));
		MULSS(XMM1, fpr.V(tregs[1]));
		SUBSS(XMM0, R(XMM1));
		MOVSS(XMM1, fpr.V(sregs[3]));
		MULSS(XMM1, fpr.V(tregs[0]));
		ADDSS(XMM0, R(XMM1));
		MOVSS(fpr.V(dregs[0]), XMM0);

		// Compute Y
		//d[1] = s[1] * t[3] + s[2] * t[0] + s[3] * t[1] - s[0] * t[2];
		MOVSS(XMM0, fpr.V(sregs[1]));
		MULSS(XMM0, fpr.V(tregs[3]));
		MOVSS(XMM1, fpr.V(sregs[2]));
		MULSS(XMM1, fpr.V(tregs[0]));
		ADDSS(XMM0, R(XMM1));
		MOVSS(XMM1, fpr.V(sregs[3]));
		MULSS(XMM1, fpr.V(tregs[1]));
		ADDSS(XMM0, R(XMM1));
		MOVSS(XMM1, fpr.V(sregs[0]));
		MULSS(XMM1, fpr.V(tregs[2]));
		SUBSS(XMM0, R(XMM1));
		MOVSS(fpr.V(dregs[1]), XMM0);

		// Compute Z
		//d[2] = s[0] * t[1] - s[1] * t[0] + s[2] * t[3] + s[3] * t[2];
		MOVSS(XMM0, fpr.V(sregs[0]));
		MULSS(XMM0, fpr.V(tregs[1]));
		MOVSS(XMM1, fpr.V(sregs[1]));
		MULSS(XMM1, fpr.V(tregs[0]));
		SUBSS(XMM0, R(XMM1));
		MOVSS(XMM1, fpr.V(sregs[2]));
		MULSS(XMM1, fpr.V(tregs[3]));
		ADDSS(XMM0, R(XMM1));
		MOVSS(XMM1, fpr.V(sregs[3]));
		MULSS(XMM1, fpr.V(tregs[2]));
		ADDSS(XMM0, R(XMM1));
		MOVSS(fpr.V(dregs[2]), XMM0);

		// Compute W
		//d[3] = -s[0] * t[0] - s[1] * t[1] - s[2] * t[2] + s[3] * t[3];
		MOVSS(XMM0, fpr.V(sregs[3]));
		MULSS(XMM0, fpr.V(tregs[3]));
		MOVSS(XMM1, fpr.V(sregs[1]));
		MULSS(XMM1, fpr.V(tregs[1]));
		SUBSS(XMM0, R(XMM1));
		MOVSS(XMM1, fpr.V(sregs[2]));
		MULSS(XMM1, fpr.V(tregs[2]));
		SUBSS(XMM0, R(XMM1));
		MOVSS(XMM1, fpr.V(sregs[0]));
		MULSS(XMM1, fpr.V(tregs[0]));
		SUBSS(XMM0, R(XMM1));
		MOVSS(fpr.V(dregs[3]), XMM0);
	}

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Vcmov(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 sregs[4], dregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixD(dregs, sz, _VD);
	int tf = (op >> 19) & 1;
	int imm3 = (op >> 16) & 7;

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);

	for (int i = 0; i < n; ++i) {
		// Simplification: Disable if overlap unsafe
		if (!IsOverlapSafeAllowS(dregs[i], i, n, sregs)) {
			DISABLE;
		}
	}

	if (imm3 < 6) {
		gpr.MapReg(MIPS_REG_VFPUCC, true, false);
		fpr.MapRegsV(dregs, sz, MAP_DIRTY);
		// Test one bit of CC. This bit decides whether none or all subregisters are copied.
		TEST(32, gpr.R(MIPS_REG_VFPUCC), Imm32(1 << imm3));
		FixupBranch skip = J_CC(tf ? CC_NZ : CC_Z, true);
		for (int i = 0; i < n; i++) {
			MOVSS(fpr.VX(dregs[i]), fpr.V(sregs[i]));
		}
		SetJumpTarget(skip);
	} else {
		gpr.MapReg(MIPS_REG_VFPUCC, true, false);
		fpr.MapRegsV(dregs, sz, MAP_DIRTY);
		// Look at the bottom four bits of CC to individually decide if the subregisters should be copied.
		for (int i = 0; i < n; i++) {
			TEST(32, gpr.R(MIPS_REG_VFPUCC), Imm32(1 << i));
			FixupBranch skip = J_CC(tf ? CC_NZ : CC_Z, true);
			MOVSS(fpr.VX(dregs[i]), fpr.V(sregs[i]));
			SetJumpTarget(skip);
		}
	}

	ApplyPrefixD(dregs, sz);

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_VecDo3(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	// Check that we can support the ops, and prepare temporary values for ops that need it.
	switch (op >> 26) {
	case 24: //VFPU0
		switch ((op >> 23) & 7) {
		case 0: // d[i] = s[i] + t[i]; break; //vadd
		case 1: // d[i] = s[i] - t[i]; break; //vsub
		case 7: // d[i] = s[i] / t[i]; break; //vdiv
			break;
		default:
			DISABLE;
		}
		break;
	case 25: //VFPU1
		switch ((op >> 23) & 7) {
		case 0: // d[i] = s[i] * t[i]; break; //vmul
			break;
		default:
			DISABLE;
		}
		break;
	case 27: //VFPU3
		switch ((op >> 23) & 7) {
		case 2:  // vmin
		case 3:  // vmax
			break;
		case 6:  // vsge
		case 7:  // vslt
			break;
		default:
			DISABLE;
		}
		break;
	default:
		DISABLE;
		break;
	}

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 sregs[4], tregs[4], dregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixT(tregs, sz, _VT);
	GetVectorRegsPrefixD(dregs, sz, _VD);

	if (fpr.TryMapDirtyInInVS(dregs, sz, sregs, sz, tregs, sz)) {
		void (XEmitter::*opFunc)(X64Reg, OpArg) = nullptr;
		bool symmetric = false;
		switch (op >> 26) {
		case 24: //VFPU0
			switch ((op >> 23) & 7) {
			case 0: // d[i] = s[i] + t[i]; break; //vadd
				opFunc = &XEmitter::ADDPS;
				symmetric = true;
				break;
			case 1: // d[i] = s[i] - t[i]; break; //vsub
				opFunc = &XEmitter::SUBPS;
				break;
			case 7: // d[i] = s[i] / t[i]; break; //vdiv
				opFunc = &XEmitter::DIVPS;
				break;
			}
			break;
		case 25: //VFPU1
			switch ((op >> 23) & 7)
			{
			case 0: // d[i] = s[i] * t[i]; break; //vmul
				opFunc = &XEmitter::MULPS;
				symmetric = true;
				break;
			}
			break;
		case 27: //VFPU3
			switch ((op >> 23) & 7)
			{
			case 2:  // vmin
				// TODO: Mishandles NaN.
				MOVAPS(XMM1, fpr.VS(sregs));
				MINPS(XMM1, fpr.VS(tregs));
				MOVAPS(fpr.VSX(dregs), R(XMM1));
				break;
			case 3:  // vmax
				// TODO: Mishandles NaN.
				MOVAPS(XMM1, fpr.VS(sregs));
				MAXPS(XMM1, fpr.VS(tregs));
				MOVAPS(fpr.VSX(dregs), R(XMM1));
				break;
			case 6:  // vsge
				// TODO: Mishandles NaN.
				MOVAPS(XMM1, fpr.VS(sregs));
				CMPPS(XMM1, fpr.VS(tregs), CMP_NLT);
				ANDPS(XMM1, M(&oneOneOneOne));
				MOVAPS(fpr.VSX(dregs), R(XMM1));
				break;
			case 7:  // vslt
				MOVAPS(XMM1, fpr.VS(sregs));
				CMPPS(XMM1, fpr.VS(tregs), CMP_LT);
				ANDPS(XMM1, M(&oneOneOneOne));
				MOVAPS(fpr.VSX(dregs), R(XMM1));
				break;
			}
			break;
		}

		if (opFunc != nullptr) {
			if (fpr.VSX(dregs) != fpr.VSX(tregs)) {
				if (fpr.VSX(dregs) != fpr.VSX(sregs)) {
					MOVAPS(fpr.VSX(dregs), fpr.VS(sregs));
				}
				(this->*opFunc)(fpr.VSX(dregs), fpr.VS(tregs));
			} else if (symmetric) {
				// We already know d = t.
				(this->*opFunc)(fpr.VSX(dregs), fpr.VS(sregs));
			} else {
				MOVAPS(XMM1, fpr.VS(sregs));
				(this->*opFunc)(XMM1, fpr.VS(tregs));
				MOVAPS(fpr.VSX(dregs), R(XMM1));
			}
		}

		ApplyPrefixD(dregs, sz);
		fpr.ReleaseSpillLocks();
		return;
	}

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(tregs, sz, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	X64Reg tempxregs[4];
	for (int i = 0; i < n; ++i)
	{
		if (!IsOverlapSafeAllowS(dregs[i], i, n, sregs, n, tregs))
		{
			// On 32-bit we only have 6 xregs for mips regs, use XMM0/XMM1 if possible.
			if (i < 2)
				tempxregs[i] = (X64Reg) (XMM0 + i);
			else
			{
				int reg = fpr.GetTempV();
				fpr.MapRegV(reg, MAP_NOINIT | MAP_DIRTY);
				fpr.SpillLockV(reg);
				tempxregs[i] = fpr.VX(reg);
			}
		}
		else
		{
			fpr.MapRegV(dregs[i], dregs[i] == sregs[i] ? MAP_DIRTY : MAP_NOINIT);
			fpr.SpillLockV(dregs[i]);
			tempxregs[i] = fpr.VX(dregs[i]);
		}
	}

	for (int i = 0; i < n; ++i)
	{
		if (!fpr.V(sregs[i]).IsSimpleReg(tempxregs[i]))
			MOVSS(tempxregs[i], fpr.V(sregs[i]));
	}

	for (int i = 0; i < n; ++i) {
		switch (op >> 26) {
		case 24: //VFPU0
			switch ((op >> 23) & 7) {
			case 0: // d[i] = s[i] + t[i]; break; //vadd
				ADDSS(tempxregs[i], fpr.V(tregs[i]));
				break;
			case 1: // d[i] = s[i] - t[i]; break; //vsub
				SUBSS(tempxregs[i], fpr.V(tregs[i]));
				break;
			case 7: // d[i] = s[i] / t[i]; break; //vdiv
				DIVSS(tempxregs[i], fpr.V(tregs[i]));
				break;
			}
			break;
		case 25: //VFPU1
			switch ((op >> 23) & 7)
			{
			case 0: // d[i] = s[i] * t[i]; break; //vmul
				MULSS(tempxregs[i], fpr.V(tregs[i]));
				break;
			}
			break;
		case 27: //VFPU3
			switch ((op >> 23) & 7)
			{
			case 2:  // vmin
				// TODO: Mishandles NaN.
				MINSS(tempxregs[i], fpr.V(tregs[i]));
				break;
			case 3:  // vmax
				// TODO: Mishandles NaN.
				MAXSS(tempxregs[i], fpr.V(tregs[i]));
				break;
			case 6:  // vsge
				// TODO: Mishandles NaN.
				CMPNLTSS(tempxregs[i], fpr.V(tregs[i]));
				ANDPS(tempxregs[i], M(&oneOneOneOne));
				break;
			case 7:  // vslt
				CMPLTSS(tempxregs[i], fpr.V(tregs[i]));
				ANDPS(tempxregs[i], M(&oneOneOneOne));
				break;
			}
			break;
		}
	}

	for (int i = 0; i < n; ++i)
	{
		if (!fpr.V(dregs[i]).IsSimpleReg(tempxregs[i]))
			MOVSS(fpr.V(dregs[i]), tempxregs[i]);
	}

	ApplyPrefixD(dregs, sz);

	fpr.ReleaseSpillLocks();
}

static float ssCompareTemp;

static u32 MEMORY_ALIGNED16( vcmpResult[4] );

static const u32 MEMORY_ALIGNED16( vcmpMask[4][4] ) = {
	{0x00000031, 0x00000000, 0x00000000, 0x00000000},
	{0x00000011, 0x00000012, 0x00000000, 0x00000000},
	{0x00000011, 0x00000012, 0x00000014, 0x00000000},
	{0x00000011, 0x00000012, 0x00000014, 0x00000018},
};

void Jit::Comp_Vcmp(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	VCondition cond = (VCondition)(op & 0xF);

	u8 sregs[4], tregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixT(tregs, sz, _VT);

	// Some, we just fall back to the interpreter.
	switch (cond) {
	case VC_EI: // c = my_isinf(s[i]); break;
	case VC_NI: // c = !my_isinf(s[i]); break;
		DISABLE;
		break;
	case VC_ES: // c = my_isnan(s[i]) || my_isinf(s[i]); break;   // Tekken Dark Resurrection
	case VC_NS: // c = !my_isnan(s[i]) && !my_isinf(s[i]); break;
	case VC_EN: // c = my_isnan(s[i]); break;
	case VC_NN: // c = !my_isnan(s[i]); break;
		if (_VS != _VT)
			DISABLE;
		break;
	default:
		break;
	}

	// First, let's get the trivial ones.

	static const int true_bits[4] = {0x31, 0x33, 0x37, 0x3f};

	if (cond == VC_TR) {
		gpr.MapReg(MIPS_REG_VFPUCC, true, true);
		OR(32, gpr.R(MIPS_REG_VFPUCC), Imm32(true_bits[n-1]));
		return;
	} else if (cond == VC_FL) {
		gpr.MapReg(MIPS_REG_VFPUCC, true, true);
		AND(32, gpr.R(MIPS_REG_VFPUCC), Imm32(~true_bits[n-1]));
		return;
	}

	if (n > 1)
		gpr.FlushLockX(ECX);

	// Start with zero in each lane for the compare to zero.
	if (cond == VC_EZ || cond == VC_NZ) {
		XORPS(XMM0, R(XMM0));
		if (n > 1) {
			XORPS(XMM1, R(XMM1));
		}
	}

	bool inverse = false;

	if (cond == VC_GE || cond == VC_GT) {
		// We flip, and we need them in regs so we don't clear the high lanes.
		fpr.SimpleRegsV(sregs, sz, 0);
		fpr.MapRegsV(tregs, sz, 0);
	} else {
		fpr.SimpleRegsV(tregs, sz, 0);
		fpr.MapRegsV(sregs, sz, 0);
	}

	// We go backwards because it's more convenient to put things in the right lanes.
	int affected_bits = (1 << 4) | (1 << 5);  // 4 and 5
	for (int i = n - 1; i >= 0; --i) {
		// Alternate between XMM0 and XMM1
		X64Reg reg = i == 1 || i == 3 ? XMM1 : XMM0;
		if ((i == 0 || i == 1) && n > 2) {
			// We need to swap lanes... this also puts them in the right place.
			SHUFPS(reg, R(reg), _MM_SHUFFLE(3, 2, 0, 1));
		}

		// Let's only handle the easy ones, and fall back on the interpreter for the rest.
		bool compareTwo = false;
		bool compareToZero = false;
		int comparison = -1;
		bool flip = false;

		switch (cond) {
		case VC_ES:
			comparison = -1;  // We will do the compare at the end. XMM1 will have the bits.
			MOVSS(reg, fpr.V(sregs[i]));
			break;

		case VC_NS:
			comparison = -1;  // We will do the compare at the end. XMM1 will have the bits.
			MOVSS(reg, fpr.V(sregs[i]));
			// Note that we do this all at once at the end.
			inverse = true;
			break;

		case VC_EN:
			comparison = CMP_UNORD;
			compareTwo = true;
			break;

		case VC_NN:
			comparison = CMP_UNORD;
			compareTwo = true;
			// Note that we do this all at once at the end.
			inverse = true;
			break;

		case VC_EQ: // c = s[i] == t[i]; break;
			comparison = CMP_EQ;
			compareTwo = true;
			break;

		case VC_LT: // c = s[i] < t[i]; break;
			comparison = CMP_LT;
			compareTwo = true;
			break;

		case VC_LE: // c = s[i] <= t[i]; break;
			comparison = CMP_LE;
			compareTwo = true;
			break;

		case VC_NE: // c = s[i] != t[i]; break;
			comparison = CMP_NEQ;
			compareTwo = true;
			break;

		case VC_GE: // c = s[i] >= t[i]; break;
			comparison = CMP_LE;
			flip = true;
			compareTwo = true;
			break;

		case VC_GT: // c = s[i] > t[i]; break;
			comparison = CMP_LT;
			flip = true;
			compareTwo = true;
			break;

		case VC_EZ: // c = s[i] == 0.0f || s[i] == -0.0f; break;
			comparison = CMP_EQ;
			compareToZero = true;
			break;

		case VC_NZ: // c = s[i] != 0; break;
			comparison = CMP_NEQ;
			compareToZero = true;
			break;

		default:
			DISABLE;
		}

		if (comparison != -1) {
			if (compareTwo) {
				if (!flip) {
					MOVSS(reg, fpr.V(sregs[i]));
					CMPSS(reg, fpr.V(tregs[i]), comparison);
				} else {
					MOVSS(reg, fpr.V(tregs[i]));
					CMPSS(reg, fpr.V(sregs[i]), comparison);
				}
			} else if (compareToZero) {
				CMPSS(reg, fpr.V(sregs[i]), comparison);
			}
		}

		affected_bits |= 1 << i;
	}

	if (n > 1) {
		XOR(32, R(ECX), R(ECX));

		// This combines them together.
		UNPCKLPS(XMM0, R(XMM1));

		// Finalize the comparison for ES/NS.
		if (cond == VC_ES || cond == VC_NS) {
			ANDPS(XMM0, M(&fourinfnan));
			PCMPEQD(XMM0, M(&fourinfnan));  // Integer comparison
			// It's inversed below for NS.
		}

		if (inverse) {
			XORPS(XMM0, M(&solidOnes));
		}
		ANDPS(XMM0, M(vcmpMask[n - 1]));
		MOVAPS(M(vcmpResult), XMM0);

		MOV(32, R(TEMPREG), M(&vcmpResult[0]));
		for (int i = 1; i < n; ++i) {
			OR(32, R(TEMPREG), M(&vcmpResult[i]));
		}

		// Aggregate the bits. Urgh, expensive. Can optimize for the case of one comparison,
		// which is the most common after all.
		CMP(32, R(TEMPREG), Imm8(affected_bits & 0x1F));
		SETcc(CC_E, R(ECX));
		SHL(32, R(ECX), Imm8(5));
		OR(32, R(TEMPREG), R(ECX));
	} else {
		// Finalize the comparison for ES/NS.
		if (cond == VC_ES || cond == VC_NS) {
			ANDPS(XMM0, M(&fourinfnan));
			PCMPEQD(XMM0, M(&fourinfnan));  // Integer comparison
			// It's inversed below for NS.
		}

		MOVD_xmm(R(TEMPREG), XMM0);
		if (inverse) {
			XOR(32, R(TEMPREG), Imm32(0xFFFFFFFF));
		}
		AND(32, R(TEMPREG), Imm32(0x31));
	}
	
	gpr.UnlockAllX();
	gpr.MapReg(MIPS_REG_VFPUCC, true, true);
	AND(32, gpr.R(MIPS_REG_VFPUCC), Imm32(~affected_bits));
	OR(32, gpr.R(MIPS_REG_VFPUCC), R(TEMPREG));

	fpr.ReleaseSpillLocks();
}

// There are no immediates for floating point, so we need to load these
// from RAM. Might as well have a table ready.
extern const float mulTableVi2f[32] = {
	1.0f/(1UL<<0),1.0f/(1UL<<1),1.0f/(1UL<<2),1.0f/(1UL<<3),
	1.0f/(1UL<<4),1.0f/(1UL<<5),1.0f/(1UL<<6),1.0f/(1UL<<7),
	1.0f/(1UL<<8),1.0f/(1UL<<9),1.0f/(1UL<<10),1.0f/(1UL<<11),
	1.0f/(1UL<<12),1.0f/(1UL<<13),1.0f/(1UL<<14),1.0f/(1UL<<15),
	1.0f/(1UL<<16),1.0f/(1UL<<17),1.0f/(1UL<<18),1.0f/(1UL<<19),
	1.0f/(1UL<<20),1.0f/(1UL<<21),1.0f/(1UL<<22),1.0f/(1UL<<23),
	1.0f/(1UL<<24),1.0f/(1UL<<25),1.0f/(1UL<<26),1.0f/(1UL<<27),
	1.0f/(1UL<<28),1.0f/(1UL<<29),1.0f/(1UL<<30),1.0f/(1UL<<31),
};

void Jit::Comp_Vi2f(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	int imm = (op >> 16) & 0x1f;
	const float *mult = &mulTableVi2f[imm];

	u8 sregs[4], dregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixD(dregs, sz, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	int tempregs[4];
	for (int i = 0; i < n; ++i) {
		if (!IsOverlapSafe(dregs[i], i, n, sregs)) {
			tempregs[i] = fpr.GetTempV();
		} else {
			tempregs[i] = dregs[i];
		}
	}

	if (*mult != 1.0f)
		MOVSS(XMM1, M(mult));
	for (int i = 0; i < n; i++) {
		fpr.MapRegV(tempregs[i], sregs[i] == dregs[i] ? MAP_DIRTY : MAP_NOINIT);
		if (fpr.V(sregs[i]).IsSimpleReg()) {
			CVTDQ2PS(fpr.VX(tempregs[i]), fpr.V(sregs[i]));
		} else {
			MOVSS(fpr.VX(tempregs[i]), fpr.V(sregs[i]));
			CVTDQ2PS(fpr.VX(tempregs[i]), R(fpr.VX(tempregs[i])));
		}
		if (*mult != 1.0f)
			MULSS(fpr.VX(tempregs[i]), R(XMM1));
	}

	for (int i = 0; i < n; ++i) {
		if (dregs[i] != tempregs[i]) {
			fpr.MapRegV(dregs[i], MAP_DIRTY | MAP_NOINIT);
			MOVSS(fpr.VX(dregs[i]), fpr.V(tempregs[i]));
		}
	}

	ApplyPrefixD(dregs, sz);
	fpr.ReleaseSpillLocks();
}

// Planning for true SIMD

// Sequence for gathering sparse registers into one SIMD:
// MOVSS(XMM0, fpr.R(sregs[0]));
// MOVSS(XMM1, fpr.R(sregs[1]));
// MOVSS(XMM2, fpr.R(sregs[2]));
// MOVSS(XMM3, fpr.R(sregs[3]));
// SHUFPS(XMM0, R(XMM1), _MM_SHUFFLE(0, 0, 0, 0));   // XMM0 = S1 S1 S0 S0
// SHUFPS(XMM2, R(XMM3), _MM_SHUFFLE(0, 0, 0, 0));   // XMM2 = S3 S3 S2 S2
// SHUFPS(XMM0, R(XMM2), _MM_SHUFFLE(2, 0, 2, 0));   // XMM0 = S3 S2 S1 S0
// Some punpckwd etc would also work.
// Alternatively, MOVSS and three PINSRD (SSE4) with mem source.
// Why PINSRD instead of INSERTPS?
// http://software.intel.com/en-us/blogs/2009/01/07/using-sse41-for-mp3-encoding-quantization

// Sequence for scattering a SIMD register to sparse registers:
// (Very serial though, better methods may be possible)
// MOVSS(fpr.R(sregs[0]), XMM0);
// SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(3, 3, 2, 1));
// MOVSS(fpr.R(sregs[1]), XMM0);
// SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(3, 3, 2, 1));
// MOVSS(fpr.R(sregs[2]), XMM0);
// SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(3, 3, 2, 1));
// MOVSS(fpr.R(sregs[3]), XMM0);
// On SSE4 we should use EXTRACTPS.

// Translation of ryg's half_to_float5_SSE2
void Jit::Comp_Vh2f(MIPSOpcode op) {
	CONDITIONAL_DISABLE;
	if (js.HasUnknownPrefix())
		DISABLE;

#define SSE_CONST4(name, val) static const u32 MEMORY_ALIGNED16(name[4]) = { (val), (val), (val), (val) }

	SSE_CONST4(mask_nosign,         0x7fff);
	SSE_CONST4(magic,               (254 - 15) << 23);
	SSE_CONST4(was_infnan,          0x7bff);
	SSE_CONST4(exp_infnan,          255 << 23);
	
#undef SSE_CONST4
	VectorSize sz = GetVecSize(op);
	VectorSize outsize;
	switch (sz) {
	case V_Single:
		outsize = V_Pair;
		break;
	case V_Pair:
		outsize = V_Quad;
		break;
	default:
		DISABLE;
	}

	u8 sregs[4], dregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixD(dregs, outsize, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);

	// Force ourselves an extra xreg as temp space.
	X64Reg tempR = fpr.GetFreeXReg();
	
	MOVSS(XMM0, fpr.V(sregs[0]));
 	if (sz != V_Single) {
		MOVSS(XMM1, fpr.V(sregs[1]));
		PUNPCKLDQ(XMM0, R(XMM1));
	}
	XORPS(XMM1, R(XMM1));
	PUNPCKLWD(XMM0, R(XMM1));

	// OK, 16 bits in each word.
	// Let's go. Deep magic here.
	MOVAPS(XMM1, R(XMM0));
	ANDPS(XMM0, M(mask_nosign)); // xmm0 = expmant
	XORPS(XMM1, R(XMM0));  // xmm1 = justsign = expmant ^ xmm0
	MOVAPS(tempR, R(XMM0));
	PCMPGTD(tempR, M(was_infnan));  // xmm2 = b_wasinfnan
	PSLLD(XMM0, 13);
	MULPS(XMM0, M(magic));  /// xmm0 = scaled
	PSLLD(XMM1, 16);  // xmm1 = sign
	ANDPS(tempR, M(exp_infnan));
	ORPS(XMM1, R(tempR));
	ORPS(XMM0, R(XMM1));

	fpr.MapRegsV(dregs, outsize, MAP_NOINIT | MAP_DIRTY);  

	// TODO: Could apply D-prefix in parallel here...

	MOVSS(fpr.V(dregs[0]), XMM0);
	SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(3, 3, 2, 1));
	MOVSS(fpr.V(dregs[1]), XMM0);

	if (sz != V_Single) {
		SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(3, 3, 2, 1));
		MOVSS(fpr.V(dregs[2]), XMM0);
		SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(3, 3, 2, 1));
		MOVSS(fpr.V(dregs[3]), XMM0);
	}

	ApplyPrefixD(dregs, outsize);
	gpr.UnlockAllX();
	fpr.ReleaseSpillLocks();
}

// The goal is to map (reversed byte order for clarity):
// AABBCCDD -> 000000AA 000000BB 000000CC 000000DD
static s8 MEMORY_ALIGNED16( vc2i_shuffle[16] ) = { -1, -1, -1, 0,  -1, -1, -1, 1,  -1, -1, -1, 2,  -1, -1, -1, 3 };
// AABBCCDD -> AAAAAAAA BBBBBBBB CCCCCCCC DDDDDDDD
static s8 MEMORY_ALIGNED16( vuc2i_shuffle[16] ) = { 0, 0, 0, 0,  1, 1, 1, 1,  2, 2, 2, 2,  3, 3, 3, 3 };

void Jit::Comp_Vx2i(MIPSOpcode op) {
	CONDITIONAL_DISABLE;
	if (js.HasUnknownPrefix())
		DISABLE;

	int bits = ((op >> 16) & 2) == 0 ? 8 : 16; // vuc2i/vc2i (0/1), vus2i/vs2i (2/3)
	bool unsignedOp = ((op >> 16) & 1) == 0; // vuc2i (0), vus2i (2)

	// vs2i or vus2i unpack pairs of 16-bit integers into 32-bit integers, with the values
	// at the top.  vus2i shifts it an extra bit right afterward.
	// vc2i and vuc2i unpack quads of 8-bit integers into 32-bit integers, with the values
	// at the top too.  vuc2i is a bit special (see below.)
	// Let's do this similarly as h2f - we do a solution that works for both singles and pairs
	// then use it for both.

	VectorSize sz = GetVecSize(op);
	VectorSize outsize;
	if (bits == 8) {
		outsize = V_Quad;
	} else {
		switch (sz) {
		case V_Single:
			outsize = V_Pair;
			break;
		case V_Pair:
			outsize = V_Quad;
			break;
		default:
			DISABLE;
		}
	}

	u8 sregs[4], dregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixD(dregs, outsize, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);

	if (bits == 16) {
		MOVSS(XMM1, fpr.V(sregs[0]));
		if (sz != V_Single) {
			MOVSS(XMM0, fpr.V(sregs[1]));
			PUNPCKLDQ(XMM1, R(XMM0));
		}

		// Unpack 16-bit words into 32-bit words, upper position, and we're done!
		PXOR(XMM0, R(XMM0));
		PUNPCKLWD(XMM0, R(XMM1));
	} else if (bits == 8) {
		if (unsignedOp) {
			// vuc2i is a bit special.  It spreads out the bits like this:
			// s[0] = 0xDDCCBBAA -> d[0] = (0xAAAAAAAA >> 1), d[1] = (0xBBBBBBBB >> 1), etc.
			MOVSS(XMM0, fpr.V(sregs[0]));
			if (cpu_info.bSSSE3) {
				// Not really different speed.  Generates a bit less code.
				PSHUFB(XMM0, M(vuc2i_shuffle));
			} else {
				// First, we change 0xDDCCBBAA to 0xDDDDCCCCBBBBAAAA.
				PUNPCKLBW(XMM0, R(XMM0));
				// Now, interleave each 16 bits so they're all 32 bits wide.
				PUNPCKLWD(XMM0, R(XMM0));
			}
		} else {
			if (cpu_info.bSSSE3) {
				MOVSS(XMM0, fpr.V(sregs[0]));
				PSHUFB(XMM0, M(vc2i_shuffle));
			} else {
				PXOR(XMM1, R(XMM1));
				MOVSS(XMM0, fpr.V(sregs[0]));
				PUNPCKLBW(XMM1, R(XMM0));
				PXOR(XMM0, R(XMM0));
				PUNPCKLWD(XMM0, R(XMM1));
			}
		}
	}

	// At this point we have the regs in the 4 lanes.
	// In the "u" mode, we need to shift it out of the sign bit.
	if (unsignedOp) {
		PSRLD(XMM0, 1);
	}

	if (fpr.TryMapRegsVS(dregs, outsize, MAP_NOINIT | MAP_DIRTY)) {
		MOVAPS(fpr.VSX(dregs), R(XMM0));
	} else {
		// Done! TODO: The rest of this should be possible to extract into a function.
		fpr.MapRegsV(dregs, outsize, MAP_NOINIT | MAP_DIRTY);

		// TODO: Could apply D-prefix in parallel here...

		MOVSS(fpr.V(dregs[0]), XMM0);
		PSRLDQ(XMM0, 4);
		MOVSS(fpr.V(dregs[1]), XMM0);

		if (outsize != V_Pair) {
			PSRLDQ(XMM0, 4);
			MOVSS(fpr.V(dregs[2]), XMM0);
			PSRLDQ(XMM0, 4);
			MOVSS(fpr.V(dregs[3]), XMM0);
		}
	}

	ApplyPrefixD(dregs, outsize);
	gpr.UnlockAllX();
	fpr.ReleaseSpillLocks();
}

extern const double mulTableVf2i[32] = {
	(1ULL<<0),(1ULL<<1),(1ULL<<2),(1ULL<<3),
	(1ULL<<4),(1ULL<<5),(1ULL<<6),(1ULL<<7),
	(1ULL<<8),(1ULL<<9),(1ULL<<10),(1ULL<<11),
	(1ULL<<12),(1ULL<<13),(1ULL<<14),(1ULL<<15),
	(1ULL<<16),(1ULL<<17),(1ULL<<18),(1ULL<<19),
	(1ULL<<20),(1ULL<<21),(1ULL<<22),(1ULL<<23),
	(1ULL<<24),(1ULL<<25),(1ULL<<26),(1ULL<<27),
	(1ULL<<28),(1ULL<<29),(1ULL<<30),(1ULL<<31),
};

static const float half = 0.5f;

static double maxIntAsDouble = (double)0x7fffffff;  // that's not equal to 0x80000000
static double minIntAsDouble = (double)(int)0x80000000;

static u32 mxcsrTemp;

void Jit::Comp_Vf2i(MIPSOpcode op) {
	CONDITIONAL_DISABLE;
	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	int imm = (op >> 16) & 0x1f;
	const double *mult = &mulTableVf2i[imm];

	int setMXCSR = -1;
	switch ((op >> 21) & 0x1f)
	{
	case 17:
		break; //z - truncate. Easy to support.
	case 16:
		setMXCSR = 0;
		break;
	case 18:
		setMXCSR = 2;
		break;
	case 19:
		setMXCSR = 1;
		break;
	}

	// Small optimization: 0 is our default mode anyway.
	if (setMXCSR == 0 && !js.hasSetRounding) {
		setMXCSR = -1;
	}
	// Except for truncate, we need to update MXCSR to our preferred rounding mode.
	if (setMXCSR != -1) {
		STMXCSR(M(&mxcsrTemp));
		MOV(32, R(TEMPREG), M(&mxcsrTemp));
		AND(32, R(TEMPREG), Imm32(~(3 << 13)));
		if (setMXCSR != 0) {
			OR(32, R(TEMPREG), Imm32(setMXCSR << 13));
		}
		MOV(32, M(&mips_->temp), R(TEMPREG));
		LDMXCSR(M(&mips_->temp));
	}

	u8 sregs[4], dregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixD(dregs, sz, _VD);

	// Really tricky to SIMD due to double precision requirement...

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_DIRTY | MAP_NOINIT);

	u8 tempregs[4];
	for (int i = 0; i < n; ++i) {
		if (!IsOverlapSafe(dregs[i], i, n, sregs)) {
			tempregs[i] = fpr.GetTempV();
		} else {
			tempregs[i] = dregs[i];
		}
	}

	if (*mult != 1.0f)
		MOVSD(XMM1, M(mult));

	fpr.MapRegsV(tempregs, sz, MAP_DIRTY | MAP_NOINIT);
	for (int i = 0; i < n; i++) {
		// Need to do this in double precision to clamp correctly as float
		// doesn't have enough precision to represent 0x7fffffff for example exactly.
		MOVSS(XMM0, fpr.V(sregs[i]));
		CVTSS2SD(XMM0, R(XMM0)); // convert to double precision
		if (*mult != 1.0f) {
			MULSD(XMM0, R(XMM1));
		}
		MINSD(XMM0, M(&maxIntAsDouble));
		MAXSD(XMM0, M(&minIntAsDouble));
		// We've set the rounding mode above, so this part's easy.
		switch ((op >> 21) & 0x1f) {
		case 16: CVTSD2SI(TEMPREG, R(XMM0)); break; //n
		case 17: CVTTSD2SI(TEMPREG, R(XMM0)); break; //z - truncate
		case 18: CVTSD2SI(TEMPREG, R(XMM0)); break; //u
		case 19: CVTSD2SI(TEMPREG, R(XMM0)); break; //d
		}
		MOVD_xmm(fpr.VX(tempregs[i]), R(TEMPREG));
	}

	for (int i = 0; i < n; ++i) {
		if (dregs[i] != tempregs[i]) {
			fpr.MapRegV(dregs[i], MAP_DIRTY | MAP_NOINIT);
			MOVSS(fpr.VX(dregs[i]), fpr.V(tempregs[i]));
			fpr.DiscardV(tempregs[i]);
		}
	}

	if (setMXCSR != -1) {
		LDMXCSR(M(&mxcsrTemp));
	}

	ApplyPrefixD(dregs, sz);
	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Vcst(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	int conNum = (op >> 16) & 0x1f;
	int vd = _VD;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 dregs[4];
	GetVectorRegsPrefixD(dregs, sz, _VD);

	MOVSS(XMM0, M(&cst_constants[conNum]));

	if (fpr.TryMapRegsVS(dregs, sz, MAP_NOINIT | MAP_DIRTY)) {
		SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(0,0,0,0));
		MOVAPS(fpr.VS(dregs), XMM0);
		fpr.ReleaseSpillLocks();
		return;
	}

	fpr.MapRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);
	for (int i = 0; i < n; i++) {
		MOVSS(fpr.V(dregs[i]), XMM0);
	}
	ApplyPrefixD(dregs, sz);
	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Vsgn(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 sregs[4], dregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixD(dregs, sz, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	X64Reg tempxregs[4];
	for (int i = 0; i < n; ++i)
	{
		if (!IsOverlapSafeAllowS(dregs[i], i, n, sregs))
		{
			int reg = fpr.GetTempV();
			fpr.MapRegV(reg, MAP_NOINIT | MAP_DIRTY);
			fpr.SpillLockV(reg);
			tempxregs[i] = fpr.VX(reg);
		}
		else
		{
			fpr.MapRegV(dregs[i], dregs[i] == sregs[i] ? MAP_DIRTY : MAP_NOINIT);
			fpr.SpillLockV(dregs[i]);
			tempxregs[i] = fpr.VX(dregs[i]);
		}
	}

	for (int i = 0; i < n; ++i)
	{
		XORPS(XMM0, R(XMM0));
		CMPEQSS(XMM0, fpr.V(sregs[i]));  // XMM0 = s[i] == 0.0f
		MOVSS(XMM1, fpr.V(sregs[i]));
		// Preserve sign bit, replace rest with ones
		ANDPS(XMM1, M(&signBitLower));
		ORPS(XMM1, M(&oneOneOneOne));
		// If really was equal to zero, zap. Note that ANDN negates the destination.
		ANDNPS(XMM0, R(XMM1));
		MOVAPS(tempxregs[i], R(XMM0));
	}

	for (int i = 0; i < n; ++i) {
		if (!fpr.V(dregs[i]).IsSimpleReg(tempxregs[i]))
			MOVSS(fpr.V(dregs[i]), tempxregs[i]);
	}

	ApplyPrefixD(dregs, sz);

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Vocp(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 sregs[4], dregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixD(dregs, sz, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	X64Reg tempxregs[4];
	for (int i = 0; i < n; ++i)
	{
		if (!IsOverlapSafeAllowS(dregs[i], i, n, sregs))
		{
			int reg = fpr.GetTempV();
			fpr.MapRegV(reg, MAP_NOINIT | MAP_DIRTY);
			fpr.SpillLockV(reg);
			tempxregs[i] = fpr.VX(reg);
		}
		else
		{
			fpr.MapRegV(dregs[i], dregs[i] == sregs[i] ? MAP_DIRTY : MAP_NOINIT);
			fpr.SpillLockV(dregs[i]);
			tempxregs[i] = fpr.VX(dregs[i]);
		}
	}

	MOVSS(XMM1, M(&one));
	for (int i = 0; i < n; ++i)
	{
		MOVSS(XMM0, R(XMM1));
		SUBSS(XMM0, fpr.V(sregs[i]));
		MOVSS(tempxregs[i], R(XMM0));
	}

	for (int i = 0; i < n; ++i) {
		if (!fpr.V(dregs[i]).IsSimpleReg(tempxregs[i]))
			MOVSS(fpr.V(dregs[i]), tempxregs[i]);
	}

	ApplyPrefixD(dregs, sz);

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Vbfy(MIPSOpcode op) {
	CONDITIONAL_DISABLE;
	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);
	if (n != 2 && n != 4) {
		DISABLE;
	}

	u8 sregs[4], dregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixD(dregs, sz, _VD);
	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	X64Reg tempxregs[4];
	for (int i = 0; i < n; ++i) {
		if (!IsOverlapSafe(dregs[i], i, n, sregs)) {
			int reg = fpr.GetTempV();
			fpr.MapRegV(reg, MAP_NOINIT | MAP_DIRTY);
			fpr.SpillLockV(reg);
			tempxregs[i] = fpr.VX(reg);
		} else {
			fpr.MapRegV(dregs[i], dregs[i] == sregs[i] ? MAP_DIRTY : MAP_NOINIT);
			fpr.SpillLockV(dregs[i]);
			tempxregs[i] = fpr.VX(dregs[i]);
		}
	}

	int subop = (op >> 16) & 0x1F;
	if (subop == 3) {
		// vbfy2
		MOVSS(tempxregs[0], fpr.V(sregs[0]));
		MOVSS(tempxregs[1], fpr.V(sregs[1]));
		MOVSS(tempxregs[2], fpr.V(sregs[0]));
		MOVSS(tempxregs[3], fpr.V(sregs[1]));
		ADDSS(tempxregs[0], fpr.V(sregs[2]));
		ADDSS(tempxregs[1], fpr.V(sregs[3]));
		SUBSS(tempxregs[2], fpr.V(sregs[2]));
		SUBSS(tempxregs[3], fpr.V(sregs[3]));
	} else if (subop == 2) {
		// vbfy1
		MOVSS(tempxregs[0], fpr.V(sregs[0]));
		MOVSS(tempxregs[1], fpr.V(sregs[0]));
		ADDSS(tempxregs[0], fpr.V(sregs[1]));
		SUBSS(tempxregs[1], fpr.V(sregs[1]));
		if (n == 4) {
			MOVSS(tempxregs[2], fpr.V(sregs[2]));
			MOVSS(tempxregs[3], fpr.V(sregs[2]));
			ADDSS(tempxregs[2], fpr.V(sregs[3]));
			SUBSS(tempxregs[3], fpr.V(sregs[3]));
		}
	} else {
		DISABLE;
	}

	for (int i = 0; i < n; ++i) {
		if (!fpr.V(dregs[i]).IsSimpleReg(tempxregs[i]))
			MOVSS(fpr.V(dregs[i]), tempxregs[i]);
	}

	ApplyPrefixD(dregs, sz);

	fpr.ReleaseSpillLocks();
}
static float sincostemp[2];

union u32float {
	u32 u;
	float f;

	operator float() const {
		return f;
	}

	inline u32float &operator *=(const float &other) {
		f *= other;
		return *this;
	}
};

#ifdef _M_X64
typedef float SinCosArg;
#else
typedef u32float SinCosArg;
#endif

void SinCos(SinCosArg angle) {
	vfpu_sincos(angle, sincostemp[0], sincostemp[1]);
}

void SinOnly(SinCosArg angle) {
	sincostemp[0] = vfpu_sin(angle);
}

void NegSinOnly(SinCosArg angle) {
	sincostemp[0] = -vfpu_sin(angle);
}

void CosOnly(SinCosArg angle) {
	sincostemp[1] = vfpu_cos(angle);
}

void ASinScaled(SinCosArg angle) {
	sincostemp[0] = asinf(angle) / M_PI_2;
}

void SinCosNegSin(SinCosArg angle) {
	vfpu_sincos(angle, sincostemp[0], sincostemp[1]);
	sincostemp[0] = -sincostemp[0];
}

void Jit::Comp_VV2Op(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	auto trigCallHelper = [this](void (*sinCosFunc)(SinCosArg), u8 sreg) {
#ifdef _M_X64
		MOVSS(XMM0, fpr.V(sreg));
		ABI_CallFunction(thunks.ProtectFunction((const void *)sinCosFunc, 0));
#else
		// Sigh, passing floats with cdecl isn't pretty, ends up on the stack.
		if (fpr.V(sreg).IsSimpleReg()) {
			MOVD_xmm(R(EAX), fpr.VX(sreg));
		} else {
			MOV(32, R(EAX), fpr.V(sreg));
		}
		CallProtectedFunction((const void *)sinCosFunc, R(EAX));
#endif
	};

	// Pre-processing: Eliminate silly no-op VMOVs, common in Wipeout Pure
	if (((op >> 16) & 0x1f) == 0 && _VS == _VD && js.HasNoPrefix()) {
		return;
	}

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 sregs[4], dregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixD(dregs, sz, _VD);

	bool canSIMD = false;
	// Some can be SIMD'd.
	switch ((op >> 16) & 0x1f) {
	case 0:  // vmov
	case 1:  // vabs
	case 2:  // vneg
		canSIMD = true;
		break;
	}

	if (canSIMD && fpr.TryMapDirtyInVS(dregs, sz, sregs, sz)) {
		switch ((op >> 16) & 0x1f) {
		case 0:  // vmov
			MOVAPS(fpr.VSX(dregs), fpr.VS(sregs));
			break;
		case 1:  // vabs
			if (dregs[0] != sregs[0])
				MOVAPS(fpr.VSX(dregs), fpr.VS(sregs));
			ANDPS(fpr.VSX(dregs), M(&noSignMask));
			break;
		case 2:  // vneg
			if (dregs[0] != sregs[0])
				MOVAPS(fpr.VSX(dregs), fpr.VS(sregs));
			XORPS(fpr.VSX(dregs), M(&signBitAll));
			break;
		}
		ApplyPrefixD(dregs, sz);
		fpr.ReleaseSpillLocks();
		return;
	}

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	X64Reg tempxregs[4];
	for (int i = 0; i < n; ++i)
	{
		if (!IsOverlapSafeAllowS(dregs[i], i, n, sregs))
		{
			int reg = fpr.GetTempV();
			fpr.MapRegV(reg, MAP_NOINIT | MAP_DIRTY);
			fpr.SpillLockV(reg);
			tempxregs[i] = fpr.VX(reg);
		}
		else
		{
			fpr.MapRegV(dregs[i], dregs[i] == sregs[i] ? MAP_DIRTY : MAP_NOINIT);
			fpr.SpillLockV(dregs[i]);
			tempxregs[i] = fpr.VX(dregs[i]);
		}
	}

	// Warning: sregs[i] and tempxregs[i] may be the same reg.
	// Helps for vmov, hurts for vrcp, etc.
	for (int i = 0; i < n; ++i)
	{
		switch ((op >> 16) & 0x1f)
		{
		case 0: // d[i] = s[i]; break; //vmov
			// Probably for swizzle.
			if (!fpr.V(sregs[i]).IsSimpleReg(tempxregs[i]))
				MOVSS(tempxregs[i], fpr.V(sregs[i]));
			break;
		case 1: // d[i] = fabsf(s[i]); break; //vabs
			if (!fpr.V(sregs[i]).IsSimpleReg(tempxregs[i]))
				MOVSS(tempxregs[i], fpr.V(sregs[i]));
			ANDPS(tempxregs[i], M(&noSignMask));
			break;
		case 2: // d[i] = -s[i]; break; //vneg
			if (!fpr.V(sregs[i]).IsSimpleReg(tempxregs[i]))
				MOVSS(tempxregs[i], fpr.V(sregs[i]));
			XORPS(tempxregs[i], M(&signBitLower));
			break;
		case 4: // if (s[i] < 0) d[i] = 0; else {if(s[i] > 1.0f) d[i] = 1.0f; else d[i] = s[i];} break;    // vsat0
			if (!fpr.V(sregs[i]).IsSimpleReg(tempxregs[i]))
				MOVSS(tempxregs[i], fpr.V(sregs[i]));

			// Zero out XMM0 if it was <= +0.0f (but skip NAN.)
			MOVSS(R(XMM0), tempxregs[i]);
			CMPLESS(XMM0, M(&zero));
			ANDNPS(XMM0, R(tempxregs[i]));

			// Retain a NAN in XMM0 (must be second operand.)
			MOVSS(tempxregs[i], M(&one));
			MINSS(tempxregs[i], R(XMM0));
			break;
		case 5: // if (s[i] < -1.0f) d[i] = -1.0f; else {if(s[i] > 1.0f) d[i] = 1.0f; else d[i] = s[i];} break;  // vsat1
			if (!fpr.V(sregs[i]).IsSimpleReg(tempxregs[i]))
				MOVSS(tempxregs[i], fpr.V(sregs[i]));

			// Check for < -1.0f, but careful of NANs.
			MOVSS(XMM1, M(&minus_one));
			MOVSS(R(XMM0), tempxregs[i]);
			CMPLESS(XMM0, R(XMM1));
			// If it was NOT less, the three ops below do nothing.
			// Otherwise, they replace the value with -1.0f.
			ANDPS(XMM1, R(XMM0));
			ANDNPS(XMM0, R(tempxregs[i]));
			ORPS(XMM0, R(XMM1));

			// Retain a NAN in XMM0 (must be second operand.)
			MOVSS(tempxregs[i], M(&one));
			MINSS(tempxregs[i], R(XMM0));
			break;
		case 16: // d[i] = 1.0f / s[i]; break; //vrcp
			MOVSS(XMM0, M(&one));
			DIVSS(XMM0, fpr.V(sregs[i]));
			MOVSS(tempxregs[i], R(XMM0));
			break;
		case 17: // d[i] = 1.0f / sqrtf(s[i]); break; //vrsq
			SQRTSS(XMM0, fpr.V(sregs[i]));
			MOVSS(tempxregs[i], M(&one));
			DIVSS(tempxregs[i], R(XMM0));
			break;
		case 18: // d[i] = sinf((float)M_PI_2 * s[i]); break; //vsin
			trigCallHelper(&SinOnly, sregs[i]);
			MOVSS(tempxregs[i], M(&sincostemp[0]));
			break;
		case 19: // d[i] = cosf((float)M_PI_2 * s[i]); break; //vcos
			trigCallHelper(&CosOnly, sregs[i]);
			MOVSS(tempxregs[i], M(&sincostemp[1]));
			break;
		case 20: // d[i] = powf(2.0f, s[i]); break; //vexp2
			DISABLE;
			break;
		case 21: // d[i] = logf(s[i])/log(2.0f); break; //vlog2
			DISABLE;
			break;
		case 22: // d[i] = sqrtf(s[i]); break; //vsqrt
			SQRTSS(tempxregs[i], fpr.V(sregs[i]));
			ANDPS(tempxregs[i], M(&noSignMask));
			break;
		case 23: // d[i] = asinf(s[i]) / M_PI_2; break; //vasin
			trigCallHelper(&ASinScaled, sregs[i]);
			MOVSS(tempxregs[i], M(&sincostemp[0]));
			break;
		case 24: // d[i] = -1.0f / s[i]; break; // vnrcp
			MOVSS(XMM0, M(&minus_one));
			DIVSS(XMM0, fpr.V(sregs[i]));
			MOVSS(tempxregs[i], R(XMM0));
			break;
		case 26: // d[i] = -sinf((float)M_PI_2 * s[i]); break; // vnsin
			trigCallHelper(&NegSinOnly, sregs[i]);
			MOVSS(tempxregs[i], M(&sincostemp[0]));
			break;
		case 28: // d[i] = 1.0f / expf(s[i] * (float)M_LOG2E); break; // vrexp2
			DISABLE;
			break;
		}
	}
	for (int i = 0; i < n; ++i)
	{
		if (!fpr.V(dregs[i]).IsSimpleReg(tempxregs[i]))
			MOVSS(fpr.V(dregs[i]), tempxregs[i]);
	}

	ApplyPrefixD(dregs, sz);

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Mftv(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	int imm = op & 0xFF;
	MIPSGPReg rt = _RT;
	switch ((op >> 21) & 0x1f)
	{
	case 3: //mfv / mfvc
		// rt = 0, imm = 255 appears to be used as a CPU interlock by some games.
		if (rt != MIPS_REG_ZERO) {
			if (imm < 128) {  //R(rt) = VI(imm);
				fpr.SimpleRegV(imm, 0);
				if (fpr.V(imm).IsSimpleReg()) {
					fpr.MapRegV(imm, 0);
					gpr.MapReg(rt, false, true);
					MOVD_xmm(gpr.R(rt), fpr.VX(imm));
				} else {
					// Let's not bother mapping the vreg.
					gpr.MapReg(rt, false, true);
					MOV(32, gpr.R(rt), fpr.V(imm));
				}
			} else if (imm < 128 + VFPU_CTRL_MAX) { //mfvc
				if (imm - 128 == VFPU_CTRL_CC) {
					if (gpr.IsImm(MIPS_REG_VFPUCC)) {
						gpr.SetImm(rt, gpr.GetImm(MIPS_REG_VFPUCC));
					} else {
						gpr.Lock(rt, MIPS_REG_VFPUCC);
						gpr.MapReg(rt, false, true);
						gpr.MapReg(MIPS_REG_VFPUCC, true, false);
						MOV(32, gpr.R(rt), gpr.R(MIPS_REG_VFPUCC));
						gpr.UnlockAll();
					}
				} else {
					// In case we have a saved prefix.
					FlushPrefixV();
					gpr.MapReg(rt, false, true);
					MOV(32, gpr.R(rt), M(&mips_->vfpuCtrl[imm - 128]));
				}
			} else {
				//ERROR - maybe need to make this value too an "interlock" value?
				_dbg_assert_msg_(CPU,0,"mfv - invalid register");
			}
		}
		break;

	case 7: //mtv
		if (imm < 128) { // VI(imm) = R(rt);
			fpr.MapRegV(imm, MAP_DIRTY | MAP_NOINIT);
			// Let's not bother mapping rt if we don't have to.
			if (gpr.IsImm(rt) && gpr.GetImm(rt) == 0) {
				XORPS(fpr.VX(imm), fpr.V(imm));
			} else {
				gpr.KillImmediate(rt, true, false);
				MOVD_xmm(fpr.VX(imm), gpr.R(rt));
			}
		} else if (imm < 128 + VFPU_CTRL_MAX) { //mtvc //currentMIPS->vfpuCtrl[imm - 128] = R(rt);
			if (imm - 128 == VFPU_CTRL_CC) {
				if (gpr.IsImm(rt)) {
					gpr.SetImm(MIPS_REG_VFPUCC, gpr.GetImm(rt));
				} else {
					gpr.Lock(rt, MIPS_REG_VFPUCC);
					gpr.MapReg(rt, true, false);
					gpr.MapReg(MIPS_REG_VFPUCC, false, true);
					MOV(32, gpr.R(MIPS_REG_VFPUCC), gpr.R(rt));
					gpr.UnlockAll();
				}
			} else {
				gpr.MapReg(rt, true, false);
				MOV(32, M(&mips_->vfpuCtrl[imm - 128]), gpr.R(rt));
			}

			// TODO: Optimization if rt is Imm?
			if (imm - 128 == VFPU_CTRL_SPREFIX) {
				js.prefixSFlag = JitState::PREFIX_UNKNOWN;
			} else if (imm - 128 == VFPU_CTRL_TPREFIX) {
				js.prefixTFlag = JitState::PREFIX_UNKNOWN;
			} else if (imm - 128 == VFPU_CTRL_DPREFIX) {
				js.prefixDFlag = JitState::PREFIX_UNKNOWN;
			}
		} else {
			//ERROR
			_dbg_assert_msg_(CPU,0,"mtv - invalid register");
		}
		break;

	default:
		DISABLE;
	}
}

void Jit::Comp_Vmfvc(MIPSOpcode op) {
	CONDITIONAL_DISABLE;
	int vs = _VS;
	int imm = op & 0xFF;
	if (imm >= 128 && imm < 128 + VFPU_CTRL_MAX) {
		fpr.MapRegV(vs, MAP_DIRTY | MAP_NOINIT);
		if (imm - 128 == VFPU_CTRL_CC) {
			gpr.MapReg(MIPS_REG_VFPUCC, true, false);
			MOVD_xmm(fpr.VX(vs), gpr.R(MIPS_REG_VFPUCC));
		} else {
			MOVSS(fpr.VX(vs), M(&mips_->vfpuCtrl[imm - 128]));
		}
		fpr.ReleaseSpillLocks();
	}
}

void Jit::Comp_Vmtvc(MIPSOpcode op) {
	CONDITIONAL_DISABLE;
	int vs = _VS;
	int imm = op & 0xFF;
	if (imm >= 128 && imm < 128 + VFPU_CTRL_MAX) {
		fpr.MapRegV(vs, 0);
		if (imm - 128 == VFPU_CTRL_CC) {
			gpr.MapReg(MIPS_REG_VFPUCC, false, true);
			MOVD_xmm(gpr.R(MIPS_REG_VFPUCC), fpr.VX(vs));
		} else {
			MOVSS(M(&mips_->vfpuCtrl[imm - 128]), fpr.VX(vs));
		}
		fpr.ReleaseSpillLocks();

		if (imm - 128 == VFPU_CTRL_SPREFIX) {
			js.prefixSFlag = JitState::PREFIX_UNKNOWN;
		} else if (imm - 128 == VFPU_CTRL_TPREFIX) {
			js.prefixTFlag = JitState::PREFIX_UNKNOWN;
		} else if (imm - 128 == VFPU_CTRL_DPREFIX) {
			js.prefixDFlag = JitState::PREFIX_UNKNOWN;
		}
	}
}

void Jit::Comp_VMatrixInit(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	MatrixSize sz = GetMtxSize(op);
	int n = GetMatrixSide(sz);

	// Not really about trying here, it will work if enabled.
	if (jo.enableVFPUSIMD) {
		VectorSize vsz = GetVectorSize(sz);
		u8 vecs[4];
		GetMatrixColumns(_VD, sz, vecs);
		for (int i = 0; i < n; i++) {
			u8 vec[4];
			GetVectorRegs(vec, vsz, vecs[i]);
			fpr.MapRegsVS(vec, vsz, MAP_NOINIT | MAP_DIRTY);
			switch ((op >> 16) & 0xF) {
			case 3:
				MOVAPS(fpr.VSX(vec), M(&identityMatrix[i]));
				break;
			case 6:
				XORPS(fpr.VSX(vec), fpr.VS(vec));
				break;
			case 7:
				MOVAPS(fpr.VSX(vec), M(&oneOneOneOne));
				break;
			}
		}
		fpr.ReleaseSpillLocks();
		return;
	}

	u8 dregs[16];
	GetMatrixRegs(dregs, sz, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	switch ((op >> 16) & 0xF) {
	case 3: // vmidt
		MOVSS(XMM0, M(&zero));
		MOVSS(XMM1, M(&one));
		for (int a = 0; a < n; a++) {
			for (int b = 0; b < n; b++) {
				MOVSS(fpr.V(dregs[a * 4 + b]), a == b ? XMM1 : XMM0);
			}
		}
		break;
	case 6: // vmzero
		MOVSS(XMM0, M(&zero));
		for (int a = 0; a < n; a++) {
			for (int b = 0; b < n; b++) {
				MOVSS(fpr.V(dregs[a * 4 + b]), XMM0);
			}
		}
		break;
	case 7: // vmone
		MOVSS(XMM0, M(&one));
		for (int a = 0; a < n; a++) {
			for (int b = 0; b < n; b++) {
				MOVSS(fpr.V(dregs[a * 4 + b]), XMM0);
			}
		}
		break;
	}

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Vmmov(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	// TODO: This probably ignores prefixes?
	if (js.HasUnknownPrefix())
		DISABLE;

	MatrixSize sz = GetMtxSize(op);
	int n = GetMatrixSide(sz);

	if (jo.enableVFPUSIMD) {
		VectorSize vsz = GetVectorSize(sz);
		u8 dest[4][4];
		MatrixOverlapType overlap = GetMatrixOverlap(_VD, _VS, sz);

		u8 vecs[4];
		if (overlap == OVERLAP_NONE) {
			GetMatrixColumns(_VD, sz, vecs);
			for (int i = 0; i < n; ++i) {
				GetVectorRegs(dest[i], vsz, vecs[i]);
			}
		} else {
			for (int i = 0; i < n; ++i) {
				fpr.GetTempVS(dest[i], vsz);
			}
		}

		GetMatrixColumns(_VS, sz, vecs);
		for (int i = 0; i < n; i++) {
			u8 vec[4];
			GetVectorRegs(vec, vsz, vecs[i]);
			fpr.MapRegsVS(vec, vsz, 0);
			fpr.MapRegsVS(dest[i], vsz, MAP_NOINIT);
			MOVAPS(fpr.VSX(dest[i]), fpr.VS(vec));
			fpr.ReleaseSpillLocks();
		}

		if (overlap != OVERLAP_NONE) {
			// Okay, move from the temps to VD now.
			GetMatrixColumns(_VD, sz, vecs);
			for (int i = 0; i < n; i++) {
				u8 vec[4];
				GetVectorRegs(vec, vsz, vecs[i]);
				fpr.MapRegsVS(vec, vsz, MAP_NOINIT);
				fpr.MapRegsVS(dest[i], vsz, 0);
				MOVAPS(fpr.VSX(vec), fpr.VS(dest[i]));
				fpr.ReleaseSpillLocks();
			}
		}

		fpr.ReleaseSpillLocks();
		return;
	}

	u8 sregs[16], dregs[16];
	GetMatrixRegs(sregs, sz, _VS);
	GetMatrixRegs(dregs, sz, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	// TODO: gas doesn't allow overlap, what does the PSP do?
	// Potentially detect overlap or the safe direction to move in, or just DISABLE?
	// This is very not optimal, blows the regcache everytime.
	u8 tempregs[16];
	for (int a = 0; a < n; a++)
	{
		for (int b = 0; b < n; b++)
		{
			u8 temp = (u8) fpr.GetTempV();
			fpr.MapRegV(temp, MAP_NOINIT | MAP_DIRTY);
			MOVSS(fpr.VX(temp), fpr.V(sregs[a * 4 + b]));
			fpr.StoreFromRegisterV(temp);
			tempregs[a * 4 + b] = temp;
		}
	}
	for (int a = 0; a < n; a++)
	{
		for (int b = 0; b < n; b++)
		{
			u8 temp = tempregs[a * 4 + b];
			fpr.MapRegV(temp, 0);
			MOVSS(fpr.V(dregs[a * 4 + b]), fpr.VX(temp));
		}
	}

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_VScl(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 sregs[4], dregs[4], scale;
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixT(&scale, V_Single, _VT);
	GetVectorRegsPrefixD(dregs, sz, _VD);

	if (fpr.TryMapDirtyInInVS(dregs, sz, sregs, sz, &scale, V_Single, true)) {
		MOVSS(XMM0, fpr.VS(&scale));
		if (sz != V_Single)
			SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(0, 0, 0, 0));
		if (dregs[0] != sregs[0]) {
			MOVAPS(fpr.VSX(dregs), fpr.VS(sregs));
		}
		MULPS(fpr.VSX(dregs), R(XMM0));
		ApplyPrefixD(dregs, sz);
		fpr.ReleaseSpillLocks();
		return;
	}

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(&scale, V_Single, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	// Move to XMM0 early, so we don't have to worry about overlap with scale.
	MOVSS(XMM0, fpr.V(scale));

	X64Reg tempxregs[4];
	for (int i = 0; i < n; ++i)
	{
		if (dregs[i] != scale || !IsOverlapSafeAllowS(dregs[i], i, n, sregs))
		{
			int reg = fpr.GetTempV();
			fpr.MapRegV(reg, MAP_NOINIT | MAP_DIRTY);
			fpr.SpillLockV(reg);
			tempxregs[i] = fpr.VX(reg);
		}
		else
		{
			fpr.MapRegV(dregs[i], dregs[i] == sregs[i] ? MAP_DIRTY : MAP_NOINIT);
			fpr.SpillLockV(dregs[i]);
			tempxregs[i] = fpr.VX(dregs[i]);
		}
	}
	for (int i = 0; i < n; ++i)
	{
		if (!fpr.V(sregs[i]).IsSimpleReg(tempxregs[i]))
			MOVSS(tempxregs[i], fpr.V(sregs[i]));
		MULSS(tempxregs[i], R(XMM0));
	}
	for (int i = 0; i < n; ++i)
	{
		if (!fpr.V(dregs[i]).IsSimpleReg(tempxregs[i]))
			MOVSS(fpr.V(dregs[i]), tempxregs[i]);
	}
	ApplyPrefixD(dregs, sz);

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Vmmul(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	// TODO: This probably ignores prefixes?
	if (js.HasUnknownPrefix())
		DISABLE;

	MatrixSize sz = GetMtxSize(op);
	VectorSize vsz = GetVectorSize(sz);
	int n = GetMatrixSide(sz);

	MatrixOverlapType soverlap = GetMatrixOverlap(_VS, _VD, sz);
	MatrixOverlapType toverlap = GetMatrixOverlap(_VT, _VD, sz);

	if (jo.enableVFPUSIMD && !soverlap && !toverlap) {
		u8 scols[4], dcols[4], tregs[16];

		int vs = _VS;
		int vd = _VD;
		int vt = _VT;

		bool transposeDest = false;
		bool transposeS = false;

		// Apparently not reliable enough yet... monster hunter hd breaks
		if (false) {
			if ((vd & 0x20) && sz == M_4x4) {
				vd ^= 0x20;
				transposeDest = true;
			}

			// Our algorithm needs a transposed S (which is the usual).
			if (!(vs & 0x20) && sz == M_4x4) {
				vs ^= 0x20;
				transposeS = true;
			}
		}

		// The T matrix we will address individually.
		GetMatrixColumns(vd, sz, dcols);
		GetMatrixRows(vs, sz, scols);
		memset(tregs, 255, sizeof(tregs));
		GetMatrixRegs(tregs, sz, vt);
		for (int i = 0; i < 16; i++) {
			if (tregs[i] != 255)
				fpr.StoreFromRegisterV(tregs[i]);
		}

		u8 scol[4][4];

		// Map all of S's columns into registers.
		for (int i = 0; i < n; i++) {
			GetVectorRegs(scol[i], vsz, scols[i]);
			fpr.MapRegsVS(scol[i], vsz, 0);
			fpr.SpillLockV(scols[i], vsz);
		}

		// Shorter than manually stuffing the registers. But it feels like ther'es room for optimization here...
		auto transposeInPlace = [=](u8 col[4][4]) {
			MOVAPS(XMM0, fpr.VS(col[0]));
			UNPCKLPS(fpr.VSX(col[0]), fpr.VS(col[2]));
			UNPCKHPS(XMM0, fpr.VS(col[2]));

			MOVAPS(fpr.VSX(col[2]), fpr.VS(col[1]));
			UNPCKLPS(fpr.VSX(col[1]), fpr.VS(col[3]));
			UNPCKHPS(fpr.VSX(col[2]), fpr.VS(col[3]));

			MOVAPS(fpr.VSX(col[3]), fpr.VS(col[0]));
			UNPCKLPS(fpr.VSX(col[0]), fpr.VS(col[1]));
			UNPCKHPS(fpr.VSX(col[3]), fpr.VS(col[1]));

			MOVAPS(fpr.VSX(col[1]), R(XMM0));
			UNPCKLPS(fpr.VSX(col[1]), fpr.VS(col[2]));
			UNPCKHPS(XMM0, fpr.VS(col[2]));

			MOVAPS(fpr.VSX(col[2]), fpr.VS(col[1]));
			MOVAPS(fpr.VSX(col[1]), fpr.VS(col[3]));
			MOVAPS(fpr.VSX(col[3]), R(XMM0));
		};

		// Some games pass in S as an E matrix (transposed). Let's just transpose the data before we do the multiplication instead.
		// This is shorter than trying to combine a discontinous matrix with lots of shufps.
		if (transposeS) {
			transposeInPlace(scol);
		}

		// Now, work our way through the matrix, loading things as we go.
		// TODO: With more temp registers, can generate much more efficient code.
		for (int i = 0; i < n; i++) {
			MOVSS(XMM1, fpr.V(tregs[4 * i]));  // TODO: AVX broadcastss to replace this and the SHUFPS
			MOVSS(XMM0, fpr.V(tregs[4 * i + 1]));
			SHUFPS(XMM1, R(XMM1), _MM_SHUFFLE(0, 0, 0, 0));
			SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(0, 0, 0, 0));
			MULPS(XMM1, fpr.VS(scol[0]));
			MULPS(XMM0, fpr.VS(scol[1]));
			ADDPS(XMM1, R(XMM0));
			for (int j = 2; j < n; j++) {
				MOVSS(XMM0, fpr.V(tregs[4 * i + j]));
				SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(0, 0, 0, 0));
				MULPS(XMM0, fpr.VS(scol[j]));
				ADDPS(XMM1, R(XMM0));
			}
			// Map the D column.
			u8 dcol[4];
			GetVectorRegs(dcol, vsz, dcols[i]);
#ifndef _M_X64
			fpr.MapRegsVS(dcol, vsz, MAP_DIRTY | MAP_NOINIT | MAP_NOLOCK);
#else
			fpr.MapRegsVS(dcol, vsz, MAP_DIRTY | MAP_NOINIT);
#endif
			MOVAPS(fpr.VS(dcol), XMM1);
		}

#ifndef _M_X64
		fpr.ReleaseSpillLocks();
#endif
		if (transposeDest) {
			u8 dcol[4][4];
			for (int i = 0; i < n; i++) {
				GetVectorRegs(dcol[i], vsz, dcols[i]);
				fpr.MapRegsVS(dcol[i], vsz, MAP_DIRTY);
			}
			transposeInPlace(dcol);
		}
		fpr.ReleaseSpillLocks();
		return;
	}

	u8 sregs[16], tregs[16], dregs[16];
	GetMatrixRegs(sregs, sz, _VS);
	GetMatrixRegs(tregs, sz, _VT);
	GetMatrixRegs(dregs, sz, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(tregs, sz, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	// Rough overlap check.
	bool overlap = false;
	if (GetMtx(_VS) == GetMtx(_VD) || GetMtx(_VT) == GetMtx(_VD)) {
		// Potential overlap (guaranteed for 3x3 or more).
		overlap = true;
	}

	if (overlap) {
		u8 tempregs[16];
		for (int a = 0; a < n; a++) {
			for (int b = 0; b < n; b++) {
				MOVSS(XMM0, fpr.V(sregs[b * 4]));
				MULSS(XMM0, fpr.V(tregs[a * 4]));
				for (int c = 1; c < n; c++) {
					MOVSS(XMM1, fpr.V(sregs[b * 4 + c]));
					MULSS(XMM1, fpr.V(tregs[a * 4 + c]));
					ADDSS(XMM0, R(XMM1));
				}
				u8 temp = (u8) fpr.GetTempV();
				fpr.MapRegV(temp, MAP_NOINIT | MAP_DIRTY);
				MOVSS(fpr.VX(temp), R(XMM0));
				fpr.StoreFromRegisterV(temp);
				tempregs[a * 4 + b] = temp;
			}
		}
		for (int a = 0; a < n; a++) {
			for (int b = 0; b < n; b++) {
				u8 temp = tempregs[a * 4 + b];
				fpr.MapRegV(temp, 0);
				MOVSS(fpr.V(dregs[a * 4 + b]), fpr.VX(temp));
			}
		}
	} else {
		for (int a = 0; a < n; a++) {
			for (int b = 0; b < n; b++) {
				MOVSS(XMM0, fpr.V(sregs[b * 4]));
				MULSS(XMM0, fpr.V(tregs[a * 4]));
				for (int c = 1; c < n; c++) {
					MOVSS(XMM1, fpr.V(sregs[b * 4 + c]));
					MULSS(XMM1, fpr.V(tregs[a * 4 + c]));
					ADDSS(XMM0, R(XMM1));
				}
				MOVSS(fpr.V(dregs[a * 4 + b]), XMM0);
			}
		}
	}
	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Vmscl(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	// TODO: This probably ignores prefixes?
	if (js.HasUnknownPrefix())
		DISABLE;

	MatrixSize sz = GetMtxSize(op);
	int n = GetMatrixSide(sz);

	u8 sregs[16], dregs[16], scale;
	GetMatrixRegs(sregs, sz, _VS);
	GetVectorRegs(&scale, V_Single, _VT);
	GetMatrixRegs(dregs, sz, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(&scale, V_Single, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	// Move to XMM0 early, so we don't have to worry about overlap with scale.
	MOVSS(XMM0, fpr.V(scale));

	// TODO: test overlap, optimize.
	u8 tempregs[16];
	for (int a = 0; a < n; a++)
	{
		for (int b = 0; b < n; b++)
		{
			u8 temp = (u8) fpr.GetTempV();
			fpr.MapRegV(temp, MAP_NOINIT | MAP_DIRTY);
			MOVSS(fpr.VX(temp), fpr.V(sregs[a * 4 + b]));
			MULSS(fpr.VX(temp), R(XMM0));
			fpr.StoreFromRegisterV(temp);
			tempregs[a * 4 + b] = temp;
		}
	}
	for (int a = 0; a < n; a++)
	{
		for (int b = 0; b < n; b++)
		{
			u8 temp = tempregs[a * 4 + b];
			fpr.MapRegV(temp, 0);
			MOVSS(fpr.V(dregs[a * 4 + b]), fpr.VX(temp));
		}
	}

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Vtfm(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	// TODO: This probably ignores prefixes?  Or maybe uses D?
	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	MatrixSize msz = GetMtxSize(op);
	int n = GetNumVectorElements(sz);
	int ins = (op >> 23) & 7;

	bool homogenous = false;
	if (n == ins) {
		n++;
		sz = (VectorSize)((int)(sz)+1);
		msz = (MatrixSize)((int)(msz)+1);
		homogenous = true;
	}
	// Otherwise, n should already be ins + 1.
	else if (n != ins + 1) {
		DISABLE;
	}

	if (jo.enableVFPUSIMD) {
		u8 scols[4], dcol[4], tregs[4];

		int vs = _VS;
		int vd = _VD;
		int vt = _VT;  // vector!

		// The T matrix we will address individually.
		GetVectorRegs(dcol, sz, vd);
		GetMatrixRows(vs, msz, scols);
		GetVectorRegs(tregs, sz, vt);
		for (int i = 0; i < n; i++) {
			fpr.StoreFromRegisterV(tregs[i]);
		}

		u8 scol[4][4];

		// Map all of S's columns into registers.
		for (int i = 0; i < n; i++) {
			GetVectorRegs(scol[i], sz, scols[i]);
			fpr.MapRegsVS(scol[i], sz, 0);
		}

		// Now, work our way through the matrix, loading things as we go.
		// TODO: With more temp registers, can generate much more efficient code.
		MOVSS(XMM1, fpr.V(tregs[0]));  // TODO: AVX broadcastss to replace this and the SHUFPS
		SHUFPS(XMM1, R(XMM1), _MM_SHUFFLE(0, 0, 0, 0));
		MULPS(XMM1, fpr.VS(scol[0]));
		for (int j = 1; j < n; j++) {
			if (!homogenous || j != n - 1) {
				MOVSS(XMM0, fpr.V(tregs[j]));
				SHUFPS(XMM0, R(XMM0), _MM_SHUFFLE(0, 0, 0, 0));
				MULPS(XMM0, fpr.VS(scol[j]));
				ADDPS(XMM1, R(XMM0));
			} else {
				ADDPS(XMM1, fpr.VS(scol[j]));
			}
		}
		// Map the D column.
		fpr.MapRegsVS(dcol, sz, MAP_DIRTY | MAP_NOINIT);
		MOVAPS(fpr.VS(dcol), XMM1);
		fpr.ReleaseSpillLocks();
		return;
	}

	u8 sregs[16], dregs[4], tregs[4];
	GetMatrixRegs(sregs, msz, _VS);
	GetVectorRegs(tregs, sz, _VT);
	GetVectorRegs(dregs, sz, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, msz, 0);
	fpr.SimpleRegsV(tregs, sz, 0);
	fpr.SimpleRegsV(dregs, sz, MAP_NOINIT | MAP_DIRTY);

	// TODO: test overlap, optimize.
	u8 tempregs[4];
	for (int i = 0; i < n; i++) {
		MOVSS(XMM0, fpr.V(sregs[i * 4]));
		MULSS(XMM0, fpr.V(tregs[0]));
		for (int k = 1; k < n; k++)
		{
			MOVSS(XMM1, fpr.V(sregs[i * 4 + k]));
			if (!homogenous || k != n - 1)
				MULSS(XMM1, fpr.V(tregs[k]));
			ADDSS(XMM0, R(XMM1));
		}

		u8 temp = (u8) fpr.GetTempV();
		fpr.MapRegV(temp, MAP_NOINIT | MAP_DIRTY);
		MOVSS(fpr.VX(temp), R(XMM0));
		fpr.StoreFromRegisterV(temp);
		tempregs[i] = temp;
	}
	for (int i = 0; i < n; i++) {
		u8 temp = tempregs[i];
		fpr.MapRegV(temp, 0);
		MOVSS(fpr.V(dregs[i]), fpr.VX(temp));
	}

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_VCrs(MIPSOpcode op) {
	DISABLE;
}

void Jit::Comp_VDet(MIPSOpcode op) {
	DISABLE;
}

// The goal is to map (reversed byte order for clarity):
// 000000AA 000000BB 000000CC 000000DD -> AABBCCDD
static s8 MEMORY_ALIGNED16( vi2xc_shuffle[16] ) = { 3, 7, 11, 15,  -1, -1, -1, -1,  -1, -1, -1, -1,  -1, -1, -1, -1 };
// 0000AAAA 0000BBBB 0000CCCC 0000DDDD -> AAAABBBB CCCCDDDD
static s8 MEMORY_ALIGNED16( vi2xs_shuffle[16] ) = { 2, 3, 6, 7,  10, 11, 14, 15,  -1, -1, -1, -1,  -1, -1, -1, -1 };

void Jit::Comp_Vi2x(MIPSOpcode op) {
	CONDITIONAL_DISABLE;
	if (js.HasUnknownPrefix())
		DISABLE;

	int bits = ((op >> 16) & 2) == 0 ? 8 : 16; // vi2uc/vi2c (0/1), vi2us/vi2s (2/3)
	bool unsignedOp = ((op >> 16) & 1) == 0; // vi2uc (0), vi2us (2)

	// These instructions pack pairs or quads of integers into 32 bits.
	// The unsigned (u) versions skip the sign bit when packing.

	VectorSize sz = GetVecSize(op);
	VectorSize outsize;
	if (bits == 8) {
		outsize = V_Single;
		if (sz != V_Quad) {
			DISABLE;
		}
	} else {
		switch (sz) {
		case V_Pair:
			outsize = V_Single;
			break;
		case V_Quad:
			outsize = V_Pair;
			break;
		default:
			DISABLE;
		}
	}

	u8 sregs[4], dregs[4];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixD(dregs, outsize, _VD);

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(dregs, outsize, MAP_NOINIT | MAP_DIRTY);

	// First, let's assemble the sregs into lanes of a single xmm reg.
	// For quad inputs, we need somewhere for the bottom regs.  Ideally dregs[0].
	X64Reg dst0 = XMM0;
	if (sz == V_Quad) {
		int vreg = dregs[0];
		if (!IsOverlapSafeAllowS(dregs[0], 0, 4, sregs)) {
			// Will be discarded on release.
			vreg = fpr.GetTempV();
		}
		fpr.MapRegV(vreg, vreg == sregs[0] ? MAP_DIRTY : MAP_NOINIT);
		fpr.SpillLockV(vreg);
		dst0 = fpr.VX(vreg);
	} else {
		// Pair, let's check if we should use dregs[0] directly.  No temp needed.
		int vreg = dregs[0];
		if (IsOverlapSafeAllowS(dregs[0], 0, 2, sregs)) {
			fpr.MapRegV(vreg, vreg == sregs[0] ? MAP_DIRTY : MAP_NOINIT);
			fpr.SpillLockV(vreg);
			dst0 = fpr.VX(vreg);
		}
	}

	if (!fpr.V(sregs[0]).IsSimpleReg(dst0)) {
		MOVSS(dst0, fpr.V(sregs[0]));
	}
	MOVSS(XMM1, fpr.V(sregs[1]));
	// With this, we have the lower half in dst0.
	PUNPCKLDQ(dst0, R(XMM1));
	if (sz == V_Quad) {
		MOVSS(XMM0, fpr.V(sregs[2]));
		MOVSS(XMM1, fpr.V(sregs[3]));
		PUNPCKLDQ(XMM0, R(XMM1));
		// Now we need to combine XMM0 into dst0.
		PUNPCKLQDQ(dst0, R(XMM0));
	} else {
		// Otherwise, we need to zero out the top 2.
		// We expect XMM1 to be zero below.
		PXOR(XMM1, R(XMM1));
		PUNPCKLQDQ(dst0, R(XMM1));
	}

	// For "u" type ops, we clamp to zero and shift off the sign bit first.
	if (unsignedOp) {
		if (cpu_info.bSSE4_1) {
			if (sz == V_Quad) {
				// Zeroed in the other case above.
				PXOR(XMM1, R(XMM1));
			}
			PMAXSD(dst0, R(XMM1));
			PSLLD(dst0, 1);
		} else {
			// Get a mask of the sign bit in dst0, then and in the values.  This clamps to 0.
			MOVDQA(XMM1, R(dst0));
			PSRAD(dst0, 31);
			PSLLD(XMM1, 1);
			PANDN(dst0, R(XMM1));
		}
	}

	// At this point, everything is aligned in the high bits of our lanes.
	if (cpu_info.bSSSE3) {
		PSHUFB(dst0, bits == 8 ? M(vi2xc_shuffle) : M(vi2xs_shuffle));
	} else {
		// Let's *arithmetically* shift in the sign so we can use saturating packs.
		PSRAD(dst0, 32 - bits);
		// XMM1 used for the high part just so there's no dependency.  It contains garbage or 0.
		PACKSSDW(dst0, R(XMM1));
		if (bits == 8) {
			PACKSSWB(dst0, R(XMM1));
		}
	}

	if (!fpr.V(dregs[0]).IsSimpleReg(dst0)) {
		MOVSS(fpr.V(dregs[0]), dst0);
	}
	if (outsize == V_Pair) {
		fpr.MapRegV(dregs[1], MAP_NOINIT | MAP_DIRTY);
		MOVDQA(fpr.V(dregs[1]), dst0);
		// Shift out the lower result to get the result we want.
		PSRLDQ(fpr.VX(dregs[1]), 4);
	}

	ApplyPrefixD(dregs, outsize);
	fpr.ReleaseSpillLocks();
}

static const float MEMORY_ALIGNED16(vavg_table[4]) = { 1.0f, 1.0f / 2.0f, 1.0f / 3.0f, 1.0f / 4.0f };

void Jit::Comp_Vhoriz(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 sregs[4], dregs[1];
	GetVectorRegsPrefixS(sregs, sz, _VS);
	GetVectorRegsPrefixD(dregs, V_Single, _VD);
	if (fpr.TryMapDirtyInVS(dregs, V_Single, sregs, sz)) {
		if (cpu_info.bSSE4_1) {
			switch (sz) {
			case V_Pair:
				MOVAPS(XMM0, fpr.VS(sregs));
				DPPS(XMM0, M(&oneOneOneOne), 0x31);
				MOVAPS(fpr.VSX(dregs), R(XMM0));
				break;
			case V_Triple:
				MOVAPS(XMM0, fpr.VS(sregs));
				DPPS(XMM0, M(&oneOneOneOne), 0x71);
				MOVAPS(fpr.VSX(dregs), R(XMM0));
				break;
			case V_Quad:
				XORPS(XMM1, R(XMM1));
				MOVAPS(XMM0, fpr.VS(sregs));
				DPPS(XMM0, M(&oneOneOneOne), 0xF1);
				// In every other case, +0.0 is selected by the mask and added.
				// But, here we need to manually add it to the result.
				ADDPS(XMM0, R(XMM1));
				MOVAPS(fpr.VSX(dregs), R(XMM0));
				break;
			default:
				DISABLE;
			}
		} else {
			switch (sz) {
			case V_Pair:
				XORPS(XMM1, R(XMM1));
				MOVAPS(XMM0, fpr.VS(sregs));
				ADDPS(XMM1, R(XMM0));
				SHUFPS(XMM1, R(XMM1), _MM_SHUFFLE(3, 2, 1, 1));
				ADDPS(XMM0, R(XMM1));
				MOVAPS(fpr.VSX(dregs), R(XMM0));
				break;
			case V_Triple:
				XORPS(XMM1, R(XMM1));
				MOVAPS(XMM0, fpr.VS(sregs));
				ADDPS(XMM1, R(XMM0));
				SHUFPS(XMM1, R(XMM1), _MM_SHUFFLE(3, 2, 1, 1));
				ADDPS(XMM0, R(XMM1));
				SHUFPS(XMM1, R(XMM1), _MM_SHUFFLE(3, 2, 1, 2));
				ADDPS(XMM0, R(XMM1));
				MOVAPS(fpr.VSX(dregs), R(XMM0));
				break;
			case V_Quad:
				XORPS(XMM1, R(XMM1));
				MOVAPS(XMM0, fpr.VS(sregs));
				// This flips the sign of any -0.000.
				ADDPS(XMM0, R(XMM1));
				MOVHLPS(XMM1, XMM0);
				ADDPS(XMM0, R(XMM1));
				MOVAPS(XMM1, R(XMM0));
				SHUFPS(XMM1, R(XMM1), _MM_SHUFFLE(1, 1, 1, 1));
				ADDPS(XMM0, R(XMM1));
				MOVAPS(fpr.VSX(dregs), R(XMM0));
				break;
			default:
				DISABLE;
			}
		}
		if (((op >> 16) & 31) == 7) { // vavg
			MULSS(fpr.VSX(dregs), M(&vavg_table[n - 1]));
		}
		ApplyPrefixD(dregs, V_Single);
		fpr.ReleaseSpillLocks();
		return;
	}

	// Flush SIMD.
	fpr.SimpleRegsV(sregs, sz, 0);
	fpr.SimpleRegsV(dregs, V_Single, MAP_NOINIT | MAP_DIRTY);

	X64Reg reg = XMM0;
	if (IsOverlapSafe(dregs[0], 0, n, sregs)) {
		fpr.MapRegV(dregs[0], dregs[0] == sregs[0] ? MAP_DIRTY : MAP_NOINIT);
		fpr.SpillLockV(dregs[0]);
		reg = fpr.VX(dregs[0]);
	}

	// We have to start zt +0.000 in case any values are -0.000.
	XORPS(reg, R(reg));
	for (int i = 0; i < n; ++i) {
		ADDSS(reg, fpr.V(sregs[i]));
	}

	switch ((op >> 16) & 31) {
	case 6:  // vfad
		break;
	case 7:  // vavg
		MULSS(reg, M(&vavg_table[n - 1]));
		break;
	}

	if (reg == XMM0) {
		MOVSS(fpr.V(dregs[0]), XMM0);
	}

	ApplyPrefixD(dregs, V_Single);
	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Viim(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	u8 dreg;
	GetVectorRegs(&dreg, V_Single, _VT);

	// Flush SIMD.
	fpr.SimpleRegsV(&dreg, V_Single, MAP_NOINIT | MAP_DIRTY);

	s32 imm = (s32)(s16)(u16)(op & 0xFFFF);
	FP32 fp;
	fp.f = (float)imm;
	MOV(32, R(TEMPREG), Imm32(fp.u));
	fpr.MapRegV(dreg, MAP_DIRTY | MAP_NOINIT);
	MOVD_xmm(fpr.VX(dreg), R(TEMPREG));

	ApplyPrefixD(&dreg, V_Single);
	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Vfim(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	if (js.HasUnknownPrefix())
		DISABLE;

	u8 dreg;
	GetVectorRegs(&dreg, V_Single, _VT);

	// Flush SIMD.
	fpr.SimpleRegsV(&dreg, V_Single, MAP_NOINIT | MAP_DIRTY);

	FP16 half;
	half.u = op & 0xFFFF;
	FP32 fval = half_to_float_fast5(half);
	MOV(32, R(TEMPREG), Imm32(fval.u));
	fpr.MapRegV(dreg, MAP_DIRTY | MAP_NOINIT);
	MOVD_xmm(fpr.VX(dreg), R(TEMPREG));

	ApplyPrefixD(&dreg, V_Single);
	fpr.ReleaseSpillLocks();
}

void Jit::CompVrotShuffle(u8 *dregs, int imm, int n, bool negSin) {
	char what[4] = { '0', '0', '0', '0' };
	if (((imm >> 2) & 3) == (imm & 3)) {
		for (int i = 0; i < 4; i++)
			what[i] = 'S';
	}
	what[(imm >> 2) & 3] = 'S';
	what[imm & 3] = 'C';

	// TODO: shufps SIMD version

	for (int i = 0; i < n; i++) {
		fpr.MapRegV(dregs[i], MAP_DIRTY | MAP_NOINIT);
		switch (what[i]) {
		case 'C': MOVSS(fpr.V(dregs[i]), XMM1); break;
		case 'S':
			MOVSS(fpr.V(dregs[i]), XMM0);
			if (negSin) {
				XORPS(fpr.VX(dregs[i]), M(&signBitLower));
			}
			break;
		case '0':
		{
			XORPS(fpr.VX(dregs[i]), fpr.V(dregs[i]));
			break;
		}
		default:
			ERROR_LOG(JIT, "Bad what in vrot");
			break;
		}
	}
}

// Very heavily used by FF:CC
void Jit::Comp_VRot(MIPSOpcode op) {
	CONDITIONAL_DISABLE;

	int vd = _VD;
	int vs = _VS;

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	u8 dregs[4];
	u8 dregs2[4];

	u32 nextOp = Memory::Read_Opcode_JIT(js.compilerPC + 4).encoding;
	int vd2 = -1;
	int imm2 = -1;
	if ((nextOp >> 26) == 60 && ((nextOp >> 21) & 0x1F) == 29 && _VS == MIPS_GET_VS(nextOp)) {
		// Pair of vrot with the same angle argument. Let's join them (can share sin/cos results).
		vd2 = MIPS_GET_VD(nextOp);
		imm2 = (nextOp >> 16) & 0x1f;
		// NOTICE_LOG(JIT, "Joint VFPU at %08x", js.blockStart);
	}

	u8 sreg;
	GetVectorRegs(dregs, sz, vd);
	if (vd2 >= 0)
		GetVectorRegs(dregs2, sz, vd2);
	GetVectorRegs(&sreg, V_Single, vs);

	// Flush SIMD.
	fpr.SimpleRegsV(&sreg, V_Single, 0);

	int imm = (op >> 16) & 0x1f;

	gpr.FlushBeforeCall();
	fpr.Flush();

	bool negSin1 = (imm & 0x10) ? true : false;

#ifdef _M_X64
	MOVSS(XMM0, fpr.V(sreg));
	ABI_CallFunction(negSin1 ? (const void *)&SinCosNegSin : (const void *)&SinCos);
#else
	// Sigh, passing floats with cdecl isn't pretty, ends up on the stack.
	ABI_CallFunctionA(negSin1 ? (const void *)&SinCosNegSin : (const void *)&SinCos, fpr.V(sreg));
#endif

	MOVSS(XMM0, M(&sincostemp[0]));
	MOVSS(XMM1, M(&sincostemp[1]));

	CompVrotShuffle(dregs, imm, n, false);
	if (vd2 != -1) {
		// If the negsin setting differs between the two joint invocations, we need to flip the second one.
		bool negSin2 = (imm2 & 0x10) ? true : false;
		CompVrotShuffle(dregs2, imm2, n, negSin1 != negSin2);
		js.compilerPC += 4;
	}
	fpr.ReleaseSpillLocks();
}

void Jit::Comp_ColorConv(MIPSOpcode op) {
	int vd = _VD;
	int vs = _VS;

	DISABLE;
#if 0
	VectorSize sz = V_Quad;
	int n = GetNumVectorElements(sz);

	switch ((op >> 16) & 3) {
	case 1:
		break;
	default:
		DISABLE;
	}

	u8 sregs[4];
	u8 dregs[1];
	GetVectorRegs(sregs, sz, vs);
	GetVectorRegs(dregs, V_Pair, vd);

	if (fpr.TryMapDirtyInVS(dregs, V_Single, sregs, sz)) {
		switch ((op >> 16) & 3) {
		case 1:  // 4444
		{
			//int a = ((in >> 24) & 0xFF) >> 4;
			//int b = ((in >> 16) & 0xFF) >> 4;
			//int g = ((in >> 8) & 0xFF) >> 4;
			//int r = ((in)& 0xFF) >> 4;
			//col = (a << 12) | (b << 8) | (g << 4) | (r);
			//PACKUSW
			break;
		}
		case 2:  // 5551
		{
			//int a = ((in >> 24) & 0xFF) >> 7;
			//int b = ((in >> 16) & 0xFF) >> 3;
			//int g = ((in >> 8) & 0xFF) >> 3;
			//int r = ((in)& 0xFF) >> 3;
			//col = (a << 15) | (b << 10) | (g << 5) | (r);
			break;
		}
		case 3:  // 565
		{
			//int b = ((in >> 16) & 0xFF) >> 3;
			//int g = ((in >> 8) & 0xFF) >> 2;
			//int r = ((in)& 0xFF) >> 3;
			//col = (b << 11) | (g << 5) | (r);
			break;
		}
	}
		DISABLE;

	// Flush SIMD.
	fpr.SimpleRegsV(&sreg, V_Pair, MAP_NOINIT | MAP_DIRTY);
	fpr.SimpleRegsV(&dreg, V_Pair, MAP_NOINIT | MAP_DIRTY);
#endif

}
}
