// $Id$

/*
TODO:
- How is 64K VRAM handled?
  VRAM size is never inspected by the command engine.
  How does a real MSX handle it?
  Mirroring of first 64K or empty memory space?
- How is extended VRAM handled?
  The current VDP implementation does not support it.
  Since it is not accessed by the renderer, it is possible allocate
  it here.
  But maybe it makes more sense to have all RAM managed by the VDP?
- Currently all VRAM access is done at the start time of a series of
  updates: currentTime is not increased until the very end of the sync
  method. It should ofcourse be updated after every read and write.
  An acceptable approximation would be an update after every pixel/byte
  operation.
*/

/*
 About NX, NY
  - for block commands NX = 0 is equivalent to NX = 512 (TODO recheck this)
    and NY = 0 is equivalent to NY = 1024
  - when NX or NY is too large and the VDP command hits the border, the
    following happens:
     - when the left or right border is hit, the line terminates
     - when the top border is hit (line 0) the command terminates
     - when the bottom border (line 511 or 1023) the command continues
       (wraps to the top)
  - in 512 lines modes (e.g. screen 7) NY is NOT limited to 512, so when
    NY > 512, part of the screen is overdrawn twice
*/

#include <stdio.h>
#include <cassert>
#include <algorithm>
#include "VDPCmdEngine.hh"
#include "EmuTime.hh"
#include "VDP.hh"
#include "VDPVRAM.hh"
#include "VDPSettings.hh"

using std::min;
using std::max;


namespace openmsx {

// Constants:
const byte DIY = 0x08;
const byte DIX = 0x04;
const byte EQ  = 0x02;
const byte MAJ = 0x01;


const byte MASK[4] = { 0x0F, 0x03, 0x0F, 0xFF };
const byte PPB[4]  = { 2, 4, 2, 1 };
const byte PPBS[4] = { 1, 2, 1, 0 };	// log2(PPB)
const word PPL[4]  = { 256, 512, 512, 256 };

//               Sprites:    On   On   Off  Off
//               Screen:     Off  On   Off  On
const int SRCH_TIMING[5] = {  92, 125,  92,  92, 0 };
const int LINE_TIMING[5] = { 120, 147, 120, 132, 0 };
const int HMMV_TIMING[5] = {  49,  65,  49,  62, 0 };
const int LMMV_TIMING[5] = {  98, 137,  98, 124, 0 };
const int YMMM_TIMING[5] = {  65, 125,  65,  68, 0 };
const int HMMM_TIMING[5] = {  92, 136,  92,  97, 0 };
const int LMMM_TIMING[5] = { 129, 197, 129, 132, 0 };



VDPCmdEngine::VDPCmdEngine(VDP *vdp_)
	: vdp(vdp_)
{
	vram = vdp->getVRAM();
	AbortCmd *abort = new AbortCmd(this, vram);
	commands[0x0] = abort;
	commands[0x1] = abort;
	commands[0x2] = abort;
	commands[0x3] = abort;
	commands[0x4] = new PointCmd(this, vram);
	commands[0x5] = new PsetCmd (this, vram);
	commands[0x6] = new SrchCmd (this, vram);
	commands[0x7] = new LineCmd (this, vram);
	commands[0x8] = new LmmvCmd (this, vram);
	commands[0x9] = new LmmmCmd (this, vram);
	commands[0xA] = new LmcmCmd (this, vram);
	commands[0xB] = new LmmcCmd (this, vram);
	commands[0xC] = new HmmvCmd (this, vram);
	commands[0xD] = new HmmmCmd (this, vram);
	commands[0xE] = new YmmmCmd (this, vram);
	commands[0xF] = new HmmcCmd (this, vram);

	status = 0;
	SX = SY = DX = DY = NX = NY = 0;
	COL = ARG = CMD = 0;
	LOG = OP_IMP;
	timingValue = vdpTiming = 0;
	statusChangeTime = EmuTime::infinity;

	VDPSettings::instance()->getCmdTiming()->addListener(this);
}

VDPCmdEngine::~VDPCmdEngine()
{
	VDPSettings::instance()->getCmdTiming()->removeListener(this);
	
	// skip 0, 1, 2
	for (int i = 3; i < 16; i++) {
		delete commands[i];
	}
}


void VDPCmdEngine::reset(const EmuTime &time)
{
	sync(time);
	status = 0;
	borderX = 0;
	scrMode = -1;
	for (int i = 0; i < 15; i++) {
		setCmdReg(i, 0, time);
	}

	updateDisplayMode(vdp->getDisplayMode(), time);
}
	
void VDPCmdEngine::updateTiming(int timing, const EmuTime &time)
{
	vdpTiming = timing;
	if (timingValue != 4) {
		sync(time);
		timingValue = timing;
	}
}

void VDPCmdEngine::update(const SettingLeafNode *setting)
{
	if (static_cast<const EnumSetting<bool>*>(setting)->getValue()) {
		timingValue = 4;
	} else {
		timingValue = vdpTiming;
	}
}

void VDPCmdEngine::setCmdReg(byte index, byte value, const EmuTime &time)
{
	sync(time);
	switch (index) {
	case 0x00: // source X low
		SX = (SX & 0x100) | value;
		break;
	case 0x01: // source X high
		SX = (SX & 0x0FF) | ((value & 0x01) << 8);
		break;
	case 0x02: // source Y low
		SY = (SY & 0x300) | value;
		break;
	case 0x03: // source Y high
		SY = (SY & 0x0FF) | ((value & 0x03) << 8);
		break;

	case 0x04: // destination X low
		DX = (DX & 0x100) | value;
		break;
	case 0x05: // destination X high
		DX = (DX & 0x0FF) | ((value & 0x01) << 8);
		break;
	case 0x06: // destination Y low
		DY = (DY & 0x300) | value;
		break;
	case 0x07: // destination Y high
		DY = (DY & 0x0FF) | ((value & 0x03) << 8);
		break;

	// TODO is DX 9 or 10 bits, at least current implementation needs
	// 10 bits (otherwise texts in UR are screwed)
	case 0x08: // number X low
		NX = (NX & 0x300) | value;
		break;
	case 0x09: // number X high
		NX = (NX & 0x0FF) | ((value & 0x03) << 8);
		break;
	case 0x0A: // number Y low
		NY = (NY & 0x300) | value;
		break;
	case 0x0B: // number Y high
		NY = (NY & 0x0FF) | ((value & 0x03) << 8);
		break;

	case 0x0C: // colour
		COL = value;
		status &= 0x7F;
		break;
	case 0x0D: // argument
		ARG = value;
		break;
	case 0x0E: // command
		LOG = (LogOp)(value & 0x0F);
		CMD = value >> 4;
		executeCommand(time);
		break;
	default:
		assert(false);
	}
}

void VDPCmdEngine::updateDisplayMode(DisplayMode mode, const EmuTime &time)
{
	int newScrMode;
	switch (mode.getBase()) {
	case DisplayMode::GRAPHIC4:
		newScrMode = 0;
		break;
	case DisplayMode::GRAPHIC5:
		newScrMode = 1;
		break;
	case DisplayMode::GRAPHIC6:
		newScrMode = 2;
		break;
	case DisplayMode::GRAPHIC7:
		newScrMode = 3;
		break;
	default:
		if (vdp->getCmdBit()) {
			newScrMode = 3;	// like GRAPHIC7
					// TODO timing might be different
		} else {
			newScrMode = -1;	// no commands
		}
		break;
	}

	if (newScrMode != scrMode) {
		sync(time);
		scrMode = newScrMode;
		// TODO for now abort cmd in progress, find out what really happens
		if (CMD) {
			PRT_DEBUG("Warning: VDP mode switch while command in progress");
			commandDone(time);
		}
	}
}

void VDPCmdEngine::executeCommand(const EmuTime &time)
{
	// V9938 ops only work in SCREEN 5-8.
	// V9958 ops work in non SCREEN 5-8 when CMD bit is set
	if (scrMode < 0) {
		commandDone(time);
		return;
	}

	//reportVdpCommand();

	// start command
	status |= 0x01;
	VDPCmd *cmd = commands[CMD];
	cmd->start(time);

	// finish command now if instantaneous command timing is active
	if (timingValue == 4) {
		cmd->execute(time);
	}
}

void VDPCmdEngine::reportVdpCommand()
{
	const char *OPS[16] = {
		"SET ","AND ","OR  ","XOR ","NOT ","NOP ","NOP ","NOP ",
		"TSET","TAND","TOR ","TXOR","TNOT","NOP ","NOP ","NOP "
	};
	const char *COMMANDS[16] = {
		" ABRT"," ????"," ????"," ????","POINT"," PSET"," SRCH"," LINE",
		" LMMV"," LMMM"," LMCM"," LMMC"," HMMV"," HMMM"," YMMM"," HMMC"
	};

	fprintf(stderr,
		"V9938: Opcode %s-%s (%d,%d)->(%d,%d),%d [%d,%d]%s\n",
		COMMANDS[CMD], OPS[LOG], SX, SY, DX, DY, COL,
		((ARG & DIX) ? -NX : NX),
		((ARG & DIY) ? -NY : NY),
		((ARG & 0x70) ? " on ExtVRAM" : ""));
}


// Inline methods first, to make sure they are actually inlined.

static inline int VDP_VRMP5(int X, int Y)
{
	return (((Y & 1023) << 7) + ((X & 255) >> 1));
}
static inline int VDP_VRMP6(int X, int Y)
{
	return (((Y & 1023) << 7) + ((X & 511) >> 2));
}
static inline int VDP_VRMP7(int X, int Y)
{
	return (((X & 2) << 15) + ((Y & 511) << 7) + ((X & 511) >> 2));
}
static inline int VDP_VRMP8(int X, int Y)
{
	return (((X & 1) << 16) + ((Y & 511) << 7) + ((X & 255) >> 1));
}


// VDPCmd

VDPCmdEngine::VDPCmd::VDPCmd(VDPCmdEngine *engine_, VDPVRAM *vram_)
	: engine(engine_), vram(vram_)
{
}

VDPCmdEngine::VDPCmd::~VDPCmd()
{
}

inline word VDPCmdEngine::VDPCmd::clipNX_1(word DX, word NX, byte ppbs)
{
	DX >>= ppbs;
	NX >>= ppbs;
	word MX = PPL[engine->scrMode] >> ppbs;
	NX = NX ? NX : MX;
	return (engine->ARG & DIX) ? min(NX, (word)(DX + 1)) :
	                             min(NX, (word)(MX - DX));
}
inline word VDPCmdEngine::VDPCmd::clipNX_2(word SX, word DX, word NX, byte ppbs)
{
	SX >>= ppbs;
	DX >>= ppbs;
	NX >>= ppbs;
	word MX = PPL[engine->scrMode] >> ppbs;
	NX = NX ? NX : MX;
	return (engine->ARG & DIX) ? min(NX, (word)(min(SX, DX) + 1)) :
	                             min(NX, (word)(MX - max(SX, DX)));
}
inline word VDPCmdEngine::VDPCmd::clipNY_1(word DY, word NY)
{
	NY = NY ? NY : 1024;
	return (engine->ARG & DIY) ? min(NY, (word)(DY + 1)) : NY;
}
inline word VDPCmdEngine::VDPCmd::clipNY_2(word SY, word DY, word NY)
{
	NY = NY ? NY : 1024;
	return (engine->ARG & DIY) ? min(NY, (word)(min(SY, DY) + 1)) : NY;
}

inline int VDPCmdEngine::VDPCmd::vramAddr(int x, int y)
{
	switch(engine->scrMode) {
	case 0: return VDP_VRMP5(x, y);
	case 1: return VDP_VRMP6(x, y);
	case 2: return VDP_VRMP7(x, y);
	case 3: return VDP_VRMP8(x, y);
	default: assert(false); return 0; // avoid warning
	}
}

inline byte VDPCmdEngine::VDPCmd::point5(int sx, int sy)
{
	return (vram->cmdReadWindow.readNP(VDP_VRMP5(sx, sy)) >>
	       (((~sx) & 1) << 2)) & 15;
}

inline byte VDPCmdEngine::VDPCmd::point6(int sx, int sy)
{
	return (vram->cmdReadWindow.readNP(VDP_VRMP6(sx, sy)) >>
	       (((~sx) & 3) << 1)) & 3;
}

inline byte VDPCmdEngine::VDPCmd::point7(int sx, int sy)
{
	return (vram->cmdReadWindow.readNP(VDP_VRMP7(sx, sy)) >>
	       (((~sx) & 1) << 2)) & 15;
}

inline byte VDPCmdEngine::VDPCmd::point8(int sx, int sy)
{
	return vram->cmdReadWindow.readNP(VDP_VRMP8(sx, sy));
}

inline byte VDPCmdEngine::VDPCmd::point(int sx, int sy)
{
	switch (engine->scrMode) {
	case 0: return point5(sx, sy);
	case 1: return point6(sx, sy);
	case 2: return point7(sx, sy);
	case 3: return point8(sx, sy);
	default: assert(false); return 0; // avoid warning
	}
}

inline void VDPCmdEngine::VDPCmd::psetLowLevel(
	int addr, byte colour, byte mask, LogOp op)
{
	switch (op) {
	case OP_IMP:
		vram->cmdWrite(
			addr, (vram->cmdWriteWindow.readNP(addr) & mask) | colour,
			currentTime);
		break;
	case OP_AND:
		vram->cmdWrite(addr,
			vram->cmdWriteWindow.readNP(addr) & (colour | mask),
			currentTime);
		break;
	case OP_OR:
		vram->cmdWrite(addr,
			vram->cmdWriteWindow.readNP(addr) | colour,
			currentTime);
		break;
	case OP_XOR:
		vram->cmdWrite(addr,
			vram->cmdWriteWindow.readNP(addr) ^ colour,
			currentTime);
		break;
	case OP_NOT:
		vram->cmdWrite(addr,
			(vram->cmdWriteWindow.readNP(addr) & mask) | ~(colour | mask),
			currentTime);
		break;
	case OP_TIMP:
		if (colour) vram->cmdWrite(addr,
			(vram->cmdWriteWindow.readNP(addr) & mask) | colour,
			currentTime);
		break;
	case OP_TAND:
		if (colour) vram->cmdWrite(addr,
			vram->cmdWriteWindow.readNP(addr) & (colour | mask),
			currentTime);
		break;
	case OP_TOR:
		if (colour) vram->cmdWrite(addr,
			vram->cmdWriteWindow.readNP(addr) | colour,
			currentTime);
		break;
	case OP_TXOR:
		if (colour) vram->cmdWrite(addr,
			vram->cmdWriteWindow.readNP(addr) ^ colour,
			currentTime);
		break;
	case OP_TNOT:
		if (colour) vram->cmdWrite(addr,
			(vram->cmdWriteWindow.readNP(addr) & mask) | ~(colour|mask),
			currentTime);
		break;
	default:
		// undefined logical operations do nothing
		break;
	}
}

inline void VDPCmdEngine::VDPCmd::pset5(int dx, int dy, byte cl, LogOp op)
{
	byte sh = ((~dx) & 1) << 2;
	psetLowLevel(VDP_VRMP5(dx, dy), cl << sh, ~(15 << sh), op);
}

inline void VDPCmdEngine::VDPCmd::pset6(int dx, int dy, byte cl, LogOp op)
{
	byte sh = ((~dx) & 3) << 1;
	psetLowLevel(VDP_VRMP6(dx, dy), cl << sh, ~(3 << sh), op);
}

inline void VDPCmdEngine::VDPCmd::pset7(int dx, int dy, byte cl, LogOp op)
{
	byte sh = ((~dx) & 1) << 2;
	psetLowLevel(VDP_VRMP7(dx, dy), cl << sh, ~(15 << sh), op);
}

inline void VDPCmdEngine::VDPCmd::pset8(int dx, int dy, byte cl, LogOp op)
{
	psetLowLevel(VDP_VRMP8(dx, dy), cl, 0, op);
}

inline void VDPCmdEngine::VDPCmd::pset(int dx, int dy, byte cl, LogOp op)
{
	switch (engine->scrMode) {
	case 0: pset5(dx, dy, cl, op); break;
	case 1: pset6(dx, dy, cl, op); break;
	case 2: pset7(dx, dy, cl, op); break;
	case 3: pset8(dx, dy, cl, op); break;
	}
}

void VDPCmdEngine::commandDone(const EmuTime& time)
{
	status &= 0xFE;
	CMD = 0;
	statusChangeTime = EmuTime::infinity;
	vram->cmdReadWindow.disable(time);
	vram->cmdWriteWindow.disable(time);
}


// Many VDP commands are executed in some kind of loop but
// essentially, there are only a few basic loop structures
// that are re-used. We define the loop structures that are
// re-used here so that they have to be entered only once.
#define pre_loop \
	while (opsCount >= delta) { \
		opsCount -= delta;

// Loop over DX, DY.
#define post__x_y() \
		ADX += TX; \
		if (--ANX == 0) { \
			engine->DY += TY; --(engine->NY); \
			ADX = engine->DX; ANX = NX; \
			if (--NY == 0) { \
				engine->commandDone(currentTime); \
				break; \
			} \
		} \
	}

// Loop over SX, DX, SY, DY.
#define post_xxyy() \
		ASX += TX; ADX += TX; \
		if (--ANX == 0) { \
			engine->SY += TY; engine->DY += TY; --(engine->NY); \
			ASX = engine->SX; ADX = engine->DX; ANX = NX; \
			if (--NY == 0) { \
				engine->commandDone(currentTime); \
				break; \
			} \
		} \
	}

// ABORT

VDPCmdEngine::AbortCmd::AbortCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: VDPCmd(engine, vram)
{
}

void VDPCmdEngine::AbortCmd::start(const EmuTime &time)
{
	engine->commandDone(time);
}

void VDPCmdEngine::AbortCmd::execute(const EmuTime &time)
{
}


// POINT

VDPCmdEngine::PointCmd::PointCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: VDPCmd(engine, vram)
{
}

void VDPCmdEngine::PointCmd::start(const EmuTime &time)
{
	currentTime = time;
	vram->cmdReadWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	vram->cmdWriteWindow.disable(currentTime);

	engine->COL = point(engine->SX, engine->SY);
	engine->commandDone(currentTime);
}

void VDPCmdEngine::PointCmd::execute(const EmuTime &time)
{
}


// PSET

VDPCmdEngine::PsetCmd::PsetCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: VDPCmd(engine, vram)
{
}

void VDPCmdEngine::PsetCmd::start(const EmuTime &time)
{
	currentTime = time;
	vram->cmdReadWindow.disable(currentTime);
	vram->cmdWriteWindow.setMask(0x1FFFF, -1 << 17, currentTime);

	byte col = engine->COL & MASK[engine->scrMode];
	pset(engine->DX, engine->DY, col, engine->LOG);
	engine->commandDone(currentTime);
}

void VDPCmdEngine::PsetCmd::execute(const EmuTime &time)
{
}


// SRCH

VDPCmdEngine::SrchCmd::SrchCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: VDPCmd(engine, vram)
{
}

void VDPCmdEngine::SrchCmd::start(const EmuTime &time)
{
	opsCount = 0;
	currentTime = time;
	vram->cmdReadWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	vram->cmdWriteWindow.disable(currentTime);
	ASX = engine->SX;
	engine->statusChangeTime = EmuTime::zero; // we can find it any moment
}

void VDPCmdEngine::SrchCmd::execute(const EmuTime &time)
{
	byte CL = engine->COL & MASK[engine->scrMode];
	char TX = (engine->ARG & DIX) ? -1 : 1;
	bool AEQ = (engine->ARG & EQ) != 0; // TODO: Do we look for "==" or "!="?
	opsCount += currentTime.getTicksTill(time);
	currentTime = time;
	int delta = SRCH_TIMING[engine->timingValue];

#define pre_srch \
		pre_loop \
		if ((
#define post_srch(MX) \
		== CL) ^ AEQ) { \
			engine->status |= 0x10; /* Border detected */ \
			engine->commandDone(currentTime); \
			engine->borderX = 0xFE00 | ASX; \
			break; \
		} \
		if ((ASX += TX) & MX) { \
			engine->status &= 0xEF; /* Border not detected */ \
			engine->commandDone(currentTime); \
			engine->borderX = 0xFE00 | ASX; \
			break; \
		} \
	}

	switch (engine->scrMode) {
	case 0: pre_srch point5(ASX, engine->SY) post_srch(256)
		break;
	case 1: pre_srch point6(ASX, engine->SY) post_srch(512)
		break;
	case 2: pre_srch point7(ASX, engine->SY) post_srch(512)
		break;
	case 3: pre_srch point8(ASX, engine->SY) post_srch(256)
		break;
	}
}


// LINE

VDPCmdEngine::LineCmd::LineCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: VDPCmd(engine, vram)
{
}

void VDPCmdEngine::LineCmd::start(const EmuTime &time)
{
	opsCount = 0;
	currentTime = time;
	vram->cmdReadWindow.disable(currentTime);
	vram->cmdWriteWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	engine->NY &= 1023;
	ASX = ((engine->NX - 1) >> 1);
	ADX = engine->DX;
	ANX = 0;
	engine->statusChangeTime = EmuTime::zero; // TODO can still be optimized
}

void VDPCmdEngine::LineCmd::execute(const EmuTime &time)
{
	byte CL = engine->COL & MASK[engine->scrMode];
	char TX = (engine->ARG & DIX) ? -1 : 1;
	char TY = (engine->ARG & DIY) ? -1 : 1;
	opsCount += currentTime.getTicksTill(time);
	currentTime = time;
	int delta = LINE_TIMING[engine->timingValue];

#define post_linexmaj(MX) \
		ADX += TX; \
		if (ASX < engine->NY) { \
			ASX += engine->NX; \
			engine->DY += TY; \
		} \
		ASX -= engine->NY; \
		ASX &= 1023; /* Mask to 10 bits range */ \
		if (ANX++ == engine->NX || (ADX & MX)) { \
			engine->commandDone(currentTime); \
			break; \
		} \
	}
#define post_lineymaj(MX) \
		engine->DY += TY; \
		if (ASX < engine->NY) { \
			ASX += engine->NX; \
			ADX += TX; \
		} \
		ASX -= engine->NY; \
		ASX &= 1023; /* Mask to 10 bits range */ \
		if (ANX++ == engine->NX || (ADX & MX)) { \
			engine->commandDone(currentTime); \
			break; \
		} \
	}

	if ((engine->ARG & MAJ) == 0) {
		// X-Axis is major direction.
		switch (engine->scrMode) {
		case 0: pre_loop pset5(ADX, engine->DY, CL, engine->LOG); post_linexmaj(256)
			break;
		case 1: pre_loop pset6(ADX, engine->DY, CL, engine->LOG); post_linexmaj(512)
			break;
		case 2: pre_loop pset7(ADX, engine->DY, CL, engine->LOG); post_linexmaj(512)
			break;
		case 3: pre_loop pset8(ADX, engine->DY, CL, engine->LOG); post_linexmaj(256)
			break;
		}
	} else {
		// Y-Axis is major direction.
		switch (engine->scrMode) {
		case 0: pre_loop pset5(ADX, engine->DY, CL, engine->LOG); post_lineymaj(256)
			break;
		case 1: pre_loop pset6(ADX, engine->DY, CL, engine->LOG); post_lineymaj(512)
			break;
		case 2: pre_loop pset7(ADX, engine->DY, CL, engine->LOG); post_lineymaj(512)
			break;
		case 3: pre_loop pset8(ADX, engine->DY, CL, engine->LOG); post_lineymaj(256)
			break;
		}
	}
}


// BlockCmd

VDPCmdEngine::BlockCmd::BlockCmd(VDPCmdEngine *engine, VDPVRAM *vram,
		                 const int* timing_)
	: VDPCmd(engine, vram), timing(timing_)
{
}

void VDPCmdEngine::BlockCmd::calcFinishTime(word NX, word NY)
{
	if (engine->CMD) {
		int ticks = ((NX * (NY - 1)) + ANX) * timing[engine->timingValue]; 
		engine->statusChangeTime = currentTime + ticks; 
	}
}


// LMMV

VDPCmdEngine::LmmvCmd::LmmvCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: BlockCmd(engine, vram, LMMV_TIMING)
{
}

void VDPCmdEngine::LmmvCmd::start(const EmuTime &time)
{
	opsCount = 0;
	currentTime = time;
	vram->cmdReadWindow.disable(currentTime);
	vram->cmdWriteWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	engine->NY &= 1023;
	word NX = clipNX_1(engine->DX, engine->NX);
	word NY = clipNY_1(engine->DY, engine->NY);
	ADX = engine->DX;
	ANX = NX;
	calcFinishTime(NX, NY);
}

void VDPCmdEngine::LmmvCmd::execute(const EmuTime &time)
{
	engine->NY &= 1023;
	word NX = clipNX_1(engine->DX, engine->NX);
	word NY = clipNY_1(engine->DY, engine->NY);
	char TX = (engine->ARG & DIX) ? -1 : 1;
	char TY = (engine->ARG & DIY) ? -1 : 1;
	ANX = clipNX_1(ADX, ANX);
	byte CL = engine->COL & MASK[engine->scrMode];
	
	opsCount += currentTime.getTicksTill(time);
	currentTime = time;
	int delta = LMMV_TIMING[engine->timingValue];

	switch (engine->scrMode) {
	case 0: pre_loop
		pset5(ADX, engine->DY, CL, engine->LOG);
		post__x_y()
		break;
	case 1: pre_loop
		pset6(ADX, engine->DY, CL, engine->LOG);
		post__x_y()
		break;
	case 2: pre_loop
		pset7(ADX, engine->DY, CL, engine->LOG);
		post__x_y()
		break;
	case 3: pre_loop
		pset8(ADX, engine->DY, CL, engine->LOG);
		post__x_y()
		break;
	}
	calcFinishTime(NX, NY);
}


// LMMM

VDPCmdEngine::LmmmCmd::LmmmCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: BlockCmd(engine, vram, LMMM_TIMING)
{
}

void VDPCmdEngine::LmmmCmd::start(const EmuTime &time)
{
	opsCount = 0;
	currentTime = time;
	vram->cmdReadWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	vram->cmdWriteWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	engine->NY &= 1023;
	word NX = clipNX_2(engine->SX, engine->DX, engine->NX);
	word NY = clipNY_2(engine->SY, engine->DY, engine->NY);
	ASX = engine->SX;
	ADX = engine->DX;
	ANX = NX;
	calcFinishTime(NX, NY);
}

void VDPCmdEngine::LmmmCmd::execute(const EmuTime &time)
{
	engine->NY &= 1023;
	word NX = clipNX_2(engine->SX, engine->DX, engine->NX);
	word NY = clipNY_2(engine->SY, engine->DY, engine->NY);
	char TX = (engine->ARG & DIX) ? -1 : 1;
	char TY = (engine->ARG & DIY) ? -1 : 1;
	ANX = clipNX_2(ASX, ADX, ANX);

	opsCount += currentTime.getTicksTill(time);
	currentTime = time;
	int delta = LMMM_TIMING[engine->timingValue];

	switch (engine->scrMode) {
	case 0: pre_loop
	        pset5(ADX, engine->DY, point5(ASX, engine->SY), engine->LOG);
		post_xxyy()
		break;
	case 1: pre_loop
		pset6(ADX, engine->DY, point6(ASX, engine->SY), engine->LOG);
		post_xxyy()
		break;
	case 2: pre_loop
		pset7(ADX, engine->DY, point7(ASX, engine->SY), engine->LOG);
		post_xxyy()
		break;
	case 3: pre_loop
		pset8(ADX, engine->DY, point8(ASX, engine->SY), engine->LOG);
		post_xxyy()
		break;
	}
	calcFinishTime(NX, NY);
}


// LMCM

VDPCmdEngine::LmcmCmd::LmcmCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: BlockCmd(engine, vram, LMMV_TIMING)
{
}

void VDPCmdEngine::LmcmCmd::start(const EmuTime &time)
{
	opsCount = 0;
	currentTime = time;
	vram->cmdReadWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	vram->cmdWriteWindow.disable(currentTime);
	engine->NY &= 1023;
	word NX = clipNX_1(engine->SX, engine->NX);
	word NY = clipNY_1(engine->SY, engine->NY);
	ASX = engine->SX;
	ANX = NX;
	calcFinishTime(NX, NY);
}

void VDPCmdEngine::LmcmCmd::execute(const EmuTime &time)
{
	engine->NY &= 1023;
	word NX = clipNX_1(engine->SX, engine->NX);
	word NY = clipNY_1(engine->SY, engine->NY);
	char TX = (engine->ARG & DIX) ? -1 : 1;
	char TY = (engine->ARG & DIY) ? -1 : 1;
	ANX = clipNX_1(ASX, ANX);
	
	opsCount += currentTime.getTicksTill(time);
	currentTime = time;
	if ((engine->status & 0x80) != 0x80) {
		engine->COL = point(ASX, engine->SY);
		opsCount -= LMMV_TIMING[engine->timingValue];
		engine->status |= 0x80;
		ASX += TX; --ANX;
		if (ANX == 0) {
			engine->SY += TY; --(engine->NY);
			ASX = engine->SX; ANX = NX;
			if (--NY == 0) {
				engine->commandDone(currentTime);
			}
		}
	} else {
		// TODO do we need to adjust opsCount?
	}
	calcFinishTime(NX, NY);
}


// LMMC

VDPCmdEngine::LmmcCmd::LmmcCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: BlockCmd(engine, vram, LMMV_TIMING)
{
}

void VDPCmdEngine::LmmcCmd::start(const EmuTime &time)
{
	opsCount = 0;
	currentTime = time;
	vram->cmdReadWindow.disable(currentTime);
	vram->cmdWriteWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	engine->NY &= 1023;
	word NX = clipNX_1(engine->DX, engine->NX);
	word NY = clipNY_1(engine->DY, engine->NY);
	ADX = engine->DX;
	ANX = NX;
	calcFinishTime(NX, NY);
}

void VDPCmdEngine::LmmcCmd::execute(const EmuTime &time)
{
	engine->NY &= 1023;
	word NX = clipNX_1(engine->DX, engine->NX);
	word NY = clipNY_1(engine->DY, engine->NY);
	char TX = (engine->ARG & DIX) ? -1 : 1;
	char TY = (engine->ARG & DIY) ? -1 : 1;
	ANX = clipNX_1(ADX, ANX);
	
	opsCount += currentTime.getTicksTill(time);
	currentTime = time;
	if ((engine->status & 0x80) != 0x80) {
		byte col = engine->COL & MASK[engine->scrMode];
		pset(ADX, engine->DY, col, engine->LOG);
		opsCount -= LMMV_TIMING[engine->timingValue];
		engine->status |= 0x80;

		ADX += TX; --ANX;
		if (ANX == 0) {
			engine->DY += TY; --(engine->NY);
			ADX = engine->DX; ANX = NX;
			if (--NY == 0) {
				engine->commandDone(currentTime);
			}
		}
	} else {
		// TODO do we need to adjust opsCount?
	}
	calcFinishTime(NX, NY);
}


// HMMV

VDPCmdEngine::HmmvCmd::HmmvCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: BlockCmd(engine, vram, HMMV_TIMING)
{
}

void VDPCmdEngine::HmmvCmd::start(const EmuTime &time)
{
	opsCount = 0;
	currentTime = time;
	vram->cmdReadWindow.disable(currentTime);
	vram->cmdWriteWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	engine->NY &= 1023;
	word NX = clipNX_1(engine->DX, engine->NX, PPBS[engine->scrMode]);
	word NY = clipNY_1(engine->DY, engine->NY);
	ADX = engine->DX;
	ANX = NX;
	calcFinishTime(NX, NY);
}

void VDPCmdEngine::HmmvCmd::execute(const EmuTime &time)
{
	engine->NY &= 1023;
	byte ppbs = PPBS[engine->scrMode];
	word NX = clipNX_1(engine->DX, engine->NX, ppbs);
	word NY = clipNY_1(engine->DY, engine->NY);
	char TX = (engine->ARG & DIX) ? -PPB[engine->scrMode] : PPB[engine->scrMode];
	char TY = (engine->ARG & DIY) ? -1 : 1;
	ANX = clipNX_1(ADX, ANX << ppbs, ppbs);
	
	opsCount += currentTime.getTicksTill(time);
	currentTime = time;
	int delta = HMMV_TIMING[engine->timingValue];

	switch (engine->scrMode) {
	case 0:
		pre_loop
		vram->cmdWrite(VDP_VRMP5(ADX, engine->DY), engine->COL, currentTime);
		post__x_y()
		break;
	case 1:
		pre_loop
		vram->cmdWrite(VDP_VRMP6(ADX, engine->DY), engine->COL, currentTime);
		post__x_y()
		break;
	case 2:
		pre_loop
		vram->cmdWrite(VDP_VRMP7(ADX, engine->DY), engine->COL, currentTime);
		post__x_y()
		break;
	case 3:
		pre_loop
		vram->cmdWrite(VDP_VRMP8(ADX, engine->DY), engine->COL, currentTime);
		post__x_y()
		break;
	}
	calcFinishTime(NX, NY);
}


// HMMM

VDPCmdEngine::HmmmCmd::HmmmCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: BlockCmd(engine, vram, HMMM_TIMING)
{
}

void VDPCmdEngine::HmmmCmd::start(const EmuTime &time)
{
	opsCount = 0;
	currentTime = time;
	vram->cmdReadWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	vram->cmdWriteWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	engine->NY &= 1023;
	word NX = clipNX_2(engine->SX, engine->DX, engine->NX, PPBS[engine->scrMode]);
	word NY = clipNY_2(engine->SY, engine->DY, engine->NY);
	ASX = engine->SX;
	ADX = engine->DX;
	ANX = NX;
	calcFinishTime(NX, NY);
}

void VDPCmdEngine::HmmmCmd::execute(const EmuTime &time)
{
	engine->NY &= 1023;
	byte ppbs = PPBS[engine->scrMode];
	word NX = clipNX_2(engine->SX, engine->DX, engine->NX, ppbs);
	word NY = clipNY_2(engine->SY, engine->DY, engine->NY);
	char TX = (engine->ARG & DIX) ? -PPB[engine->scrMode] : PPB[engine->scrMode];
	char TY = (engine->ARG & DIY) ? -1 : 1;
	ANX = clipNX_2(ASX, ADX, ANX << ppbs, ppbs);

	opsCount += currentTime.getTicksTill(time);
	currentTime = time;
	int delta = HMMM_TIMING[engine->timingValue];

	switch (engine->scrMode) {
	case 0:
		pre_loop
		vram->cmdWrite(
			VDP_VRMP5(ADX, engine->DY),
			vram->cmdReadWindow.readNP(VDP_VRMP5(ASX, engine->SY)),
			currentTime);
		post_xxyy()
		break;
	case 1:
		pre_loop
		vram->cmdWrite(
			VDP_VRMP6(ADX, engine->DY),
			vram->cmdReadWindow.readNP(VDP_VRMP6(ASX, engine->SY)),
			currentTime);
		post_xxyy()
		break;
	case 2:
		pre_loop
		vram->cmdWrite(
			VDP_VRMP7(ADX, engine->DY),
			vram->cmdReadWindow.readNP(VDP_VRMP7(ASX, engine->SY)),
			currentTime);
		post_xxyy()
		break;
	case 3:
		pre_loop
		vram->cmdWrite(
			VDP_VRMP8(ADX, engine->DY),
			vram->cmdReadWindow.readNP(VDP_VRMP8(ASX, engine->SY)),
			currentTime);
		post_xxyy()
		break;
	}
	calcFinishTime(NX, NY);
}


// YMMM

VDPCmdEngine::YmmmCmd::YmmmCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: BlockCmd(engine, vram, YMMM_TIMING)
{
}

void VDPCmdEngine::YmmmCmd::start(const EmuTime &time)
{
	opsCount = 0;
	currentTime = time;
	vram->cmdReadWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	vram->cmdWriteWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	engine->NY &= 1023;
	word NX = clipNX_1(engine->DX, 512, PPBS[engine->scrMode]); // large enough so that it gets clipped
	word NY = clipNY_2(engine->SY, engine->DY, engine->NY);
	ADX = engine->DX;
	ANX = NX;
	calcFinishTime(NX, NY);
}

void VDPCmdEngine::YmmmCmd::execute(const EmuTime &time)
{
	engine->NY &= 1023;
	word NX = clipNX_1(engine->DX, 512, PPBS[engine->scrMode]); // large enough so that it gets clipped
	word NY = clipNY_2(engine->SY, engine->DY, engine->NY);
	char TX = (engine->ARG & DIX) ? -PPB[engine->scrMode] : PPB[engine->scrMode];
	char TY = (engine->ARG & DIY) ? -1 : 1;
	ANX = clipNX_1(ADX, 512, PPBS[engine->scrMode]);

	opsCount += currentTime.getTicksTill(time);
	currentTime = time;
	int delta = YMMM_TIMING[engine->timingValue];

	switch (engine->scrMode) {
	case 0:
		pre_loop
		vram->cmdWrite(
			VDP_VRMP5(ADX, engine->DY),
			vram->cmdReadWindow.readNP(VDP_VRMP5(ADX, engine->SY)),
			currentTime);
		post_xxyy()
		break;
	case 1:
		pre_loop
		vram->cmdWrite(
			VDP_VRMP6(ADX, engine->DY),
			vram->cmdReadWindow.readNP(VDP_VRMP6(ADX, engine->SY)),
			currentTime);
		post_xxyy()
		break;
	case 2:
		pre_loop
		vram->cmdWrite(
			VDP_VRMP7(ADX, engine->DY),
			vram->cmdReadWindow.readNP(VDP_VRMP7(ADX, engine->SY)),
			currentTime);
		post_xxyy()
		break;
	case 3:
		pre_loop
		vram->cmdWrite(
			VDP_VRMP8(ADX, engine->DY),
			vram->cmdReadWindow.readNP(VDP_VRMP8(ADX, engine->SY)),
			currentTime);
		post_xxyy()
		break;
	}
	calcFinishTime(NX, NY);
}


// HMMC

VDPCmdEngine::HmmcCmd::HmmcCmd(VDPCmdEngine *engine, VDPVRAM *vram)
	: BlockCmd(engine, vram, HMMV_TIMING)
{
}

void VDPCmdEngine::HmmcCmd::start(const EmuTime &time)
{
	opsCount = 0;
	currentTime = time;
	vram->cmdReadWindow.disable(currentTime);
	vram->cmdWriteWindow.setMask(0x1FFFF, -1 << 17, currentTime);
	engine->NY &= 1023;
	word NX = clipNX_1(engine->DX, engine->NX, PPBS[engine->scrMode]);
	word NY = clipNY_1(engine->DY, engine->NY);
	ADX = engine->DX;
	ANX = NX;
	calcFinishTime(NX, NY);
}

void VDPCmdEngine::HmmcCmd::execute(const EmuTime &time)
{
	engine->NY &= 1023;
	byte ppbs = PPBS[engine->scrMode];
	word NX = clipNX_1(engine->DX, engine->NX, ppbs);
	word NY = clipNY_1(engine->DY, engine->NY);
	char TX = (engine->ARG & DIX) ? -PPB[engine->scrMode] : PPB[engine->scrMode];
	char TY = (engine->ARG & DIY) ? -1 : 1;
	ANX = clipNX_1(ADX, ANX << ppbs, ppbs);
	
	opsCount += currentTime.getTicksTill(time);
	currentTime = time;
	if ((engine->status & 0x80) != 0x80) {
		vram->cmdWrite(vramAddr(ADX, engine->DY), engine->COL, currentTime);
		opsCount -= HMMV_TIMING[engine->timingValue];
		engine->status |= 0x80;

		ADX += TX; --ANX;
		if (ANX == 0) {
			engine->DY += TY; --(engine->NY);
			ADX = engine->DX; ANX = NX;
			if (--NY == 0) {
				engine->commandDone(currentTime);
			}
		}
	} else {
		// TODO do we need to adjust opsCount?
	}
	calcFinishTime(NX, NY);
}

} // namespace openmsx
