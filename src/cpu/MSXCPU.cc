// $Id$

#include <cassert>
#include "MSXCPU.hh"
#include "MSXConfig.hh"
#include "MSXCPUInterface.hh"
#include "DebugInterface.hh"
#include "CPU.hh"


namespace openmsx {

MSXCPU::MSXCPU(Device *config, const EmuTime &time)
	: MSXDevice(config, time),
	  z80 (MSXCPUInterface::instance(), time),
	  r800(MSXCPUInterface::instance(), time)
{
	activeCPU = &z80;	// setActiveCPU(CPU_Z80);
	reset(time);
}

MSXCPU::~MSXCPU()
{
}

MSXCPU* MSXCPU::instance()
{
	// MSXCPU is a MSXDevice and is automatically deleted
	static MSXCPU* oneInstance = NULL;
	if (oneInstance == NULL) {
		Device* config = MSXConfig::instance()->getDeviceById("cpu");
		EmuTime zero;
		oneInstance = new MSXCPU(config, zero);
	}
	return oneInstance;
}

void MSXCPU::init(Scheduler* scheduler)
{
	z80.init(scheduler);
	r800.init(scheduler);
}

void MSXCPU::reset(const EmuTime &time)
{
	MSXDevice::reset(time);
	z80.reset(time);
	r800.reset(time);
}


void MSXCPU::setActiveCPU(CPUType cpu)
{
	CPU *newCPU;
	switch (cpu) {
		case CPU_Z80:
			PRT_DEBUG("Active CPU: Z80");
			newCPU = &z80;
			break;
		case CPU_R800:
			PRT_DEBUG("Active CPU: R800");
			newCPU = &r800;
			break;
		default:
			assert(false);
			newCPU = NULL;	// prevent warning
	}
	if (newCPU != activeCPU) {
		const EmuTime &currentTime = activeCPU->getCurrentTime();
		const EmuTime &targetTime  = activeCPU->getTargetTime();
		activeCPU->setTargetTime(currentTime);	// stop current CPU
		newCPU->setCurrentTime(currentTime);
		newCPU->setTargetTime(targetTime);
		newCPU->invalidateCache(0x0000, 0x10000/CPU::CACHE_LINE_SIZE);
		activeCPU = newCPU;
	}
}

void MSXCPU::executeUntilTarget(const EmuTime &time)
{
	activeCPU->executeUntilTarget(time);
}

void MSXCPU::setTargetTime(const EmuTime &time)
{
	activeCPU->setTargetTime(time);
}

const EmuTime &MSXCPU::getTargetTime() const
{
	return activeCPU->getTargetTime();
}

const EmuTime &MSXCPU::getCurrentTime() const
{
	return activeCPU->getCurrentTime();
}


void MSXCPU::invalidateCache(word start, int num)
{
	activeCPU->invalidateCache(start, num);
}

void MSXCPU::raiseIRQ()
{
	z80.raiseIRQ();
	r800.raiseIRQ();
}
void MSXCPU::lowerIRQ()
{
	z80.lowerIRQ();
	r800.lowerIRQ();
}

bool MSXCPU::isR800Active()
{
	return activeCPU == &r800;
}

void MSXCPU::wait(const EmuTime &time)
{
	activeCPU->wait(time);
}


// DebugInterface

static string regNames[] = {
	"AF",  "BC",  "DE",  "HL",
	"AF2", "BC2", "DE2", "HL2",
	"IX",  "IY",  "PC",  "SP",
	"IR"
};

dword MSXCPU::getDataSize() const
{
	return 26; // number of 8 bits registers (16 bits = 2 registers)
}

const string MSXCPU::getRegisterName(dword regNr) const
{
	assert(regNr < getDataSize());
	return regNames[regNr / 2];
}

dword MSXCPU::getRegisterNumber(const string& regName) const
{
	for (int i = 0; i < (26 / 2); ++i) {
		if (regName == regNames[i]) {
			return i * 2;
		}
	}
	return 0;
}

byte MSXCPU::readDebugData(dword address) const
{
	CPU::CPURegs* regs = &activeCPU->R; 
	const CPU::z80regpair* registers[] = {
		&regs->AF,  &regs->BC,  &regs->DE,  &regs->HL, 
		&regs->AF2, &regs->BC2, &regs->DE2, &regs->HL2, 
		&regs->IX,  &regs->IY,  &regs->PC,  &regs->SP
	};

	assert(address < getDataSize());
	switch (address) {
	case 24:
		return regs->I;
	case 25:
		return regs->R;
	default:
		if (address & 1) {
			return registers[address / 2]->B.l;
		} else {
			return registers[address / 2]->B.h;
		}
		break;
	}
}

const string& MSXCPU::getDeviceName() const
{
	static const string NAME("cpu");
	return NAME;
}

} // namespace openmsx
