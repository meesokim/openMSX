// $Id$

// This class implements the PPI (8255)
//
//   PPI    MSX-I/O  Direction  MSX-Function
//  PortA    0xA8      Out     Memory primary slot register
//  PortB    0xA9      In      Keyboard column inputs
//  PortC    0xAA      Out     Keyboard row select / CAPS / CASo / CASm / SND
//  Control  0xAB     In/Out   Mode select for PPI
//
//  Direction indicates the direction normally used on MSX.
//  Reading from an output port returns the last written byte.
//  Writing to an input port has no immediate effect.
//
//  PortA combined with upper half of PortC form groupA
//  PortB               lower                    groupB
//  GroupA can be in programmed in 3 modes
//   - basic input/output
//   - strobed input/output
//   - bidirectional
//  GroupB can only use the first two modes.
//  Only the first mode is used on MSX, only this mode is implemented yet.
//
//  for more detail see
//    http://.../8255.pdf    TODO: fix url

#ifndef __MSXPPI_HH__
#define __MSXPPI_HH__

#include "MSXDevice.hh"
#include "MSXMotherBoard.hh"
#include "emutime.hh"
#include "I8255.hh"


class MSXPPI : public MSXDevice, I8255Interface
{
	// MSXDevice
	public:
		MSXPPI(); 
		~MSXPPI(); 
		static MSXDevice *instance();
		
		void init();
		void reset();
		byte readIO(byte port, Emutime &time);
		void writeIO(byte port, byte value, Emutime &time);
	private:
		static MSXPPI *volatile oneInstance;
		I8255 *i8255;
	
	// I8255Interface
	public:
		byte readA();
		byte readB();
		nibble readC0();
		nibble readC1();
		void writeA(byte value);
		void writeB(byte value);
		void writeC0(nibble value);
		void writeC1(nibble value);
	
	private:
		void keyGhosting();
		bool keyboardGhosting;

		byte MSXKeyMatrix[16];
		int selectedRow;

	friend class Inputs;
};
#endif
