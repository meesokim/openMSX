// $Id$

#ifndef __CASSETTEPLAYER_HH__
#define __CASSETTEPLAYER_HH__

#include "CassetteDevice.hh"
#include "EmuTime.hh"
#include "Command.hh"
#include "CommandLineParser.hh"
#include "SoundDevice.hh"

using std::string;
using std::vector;
using std::list;

class FileContext;
class CassetteImage;

class MSXCassettePlayerCLI : public CLIOption, public CLIFileType
{
	public:
		MSXCassettePlayerCLI();
		virtual void parseOption(const string &option,
				list<string> &cmdLine);
		virtual const string& optionHelp() const;
		virtual void parseFileType(const string &filename);
		virtual const string& fileTypeHelp() const;
};


class CassettePlayer : public CassetteDevice, private Command, public SoundDevice
{
	public:
		CassettePlayer();
		virtual ~CassettePlayer();
		
		void insertTape(FileContext *context,
		                const string &filename);
		void removeTape();
		
		// CassetteDevice
		virtual void setMotor(bool status, const EmuTime &time);
		virtual short readSample(const EmuTime &time);
		virtual void writeWave(short *buf, int length);
		virtual int getWriteSampleRate();

		// Pluggable
		virtual const string &getName() const;

		// SoundDevice
		virtual void setInternalVolume(short newVolume);
		virtual void setSampleRate(int sampleRate);
		virtual int* updateBuffer(int length);

	private:
		void rewind();
		void updatePosition(const EmuTime &time);
		short getSample(const EmuTime &time);

		CassetteImage *cassette;
		bool motor;
		EmuTime tapeTime;
		EmuTime prevTime;

		// Tape Command
		virtual void execute(const vector<string> &tokens);
		virtual void help   (const vector<string> &tokens) const;
		virtual void tabCompletion(vector<string> &tokens) const;

		// SoundDevice
		int *buffer;
		short volume;
		EmuDuration delta;
		EmuTime playTapeTime;
};

#endif
