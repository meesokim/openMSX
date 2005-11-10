// $Id$

#ifndef CHARACTERCONVERTER_HH
#define CHARACTERCONVERTER_HH

#include "DisplayMode.hh"
#include "openmsx.hh"
#include <bitset>

namespace openmsx {

class VDP;
class VDPVRAM;


/** Utility class for converting VRAM contents to host pixels.
  */
template <class Pixel>
class CharacterConverter
{
public:
	/** Create a new bitmap scanline converter.
	  * @param vdp The VDP of which the VRAM will be converted.
	  * @param palFg Pointer to 16-entries array that specifies
	  *   VDP foreground colour index to host pixel mapping.
	  *   This is kept as a pointer, so any changes to the palette
	  *   are immediately picked up by convertLine.
	  * @param palBg Pointer to 16-entries array that specifies
	  *   VDP background colour index to host pixel mapping.
	  *   This is kept as a pointer, so any changes to the palette
	  *   are immediately picked up by convertLine.
	  */
	CharacterConverter(VDP& vdp, const Pixel* palFg, const Pixel* palBg);

	/** Convert a line of V9938 VRAM to 512 host pixels.
	  * Call this method in non-planar display modes (Graphic4 and Graphic5).
	  * @param linePtr Pointer to array where host pixels will be written to.
	  * @param line Display line number [0..255].
	  */
	inline void convertLine(Pixel* linePtr, int line)
	{
		(this->*renderMethod)(linePtr, line);
	}

private:
	/** Convert a monochrome 8*1 character block row to host pixels.
	  * @param blockPtr Pointer to array where host pixels will be written to.
	  * @param pattern Pattern to convert.
	  * @param fg Foreground colour.
	  * @param bg Background colour.
	  */
	inline void convertBlockRow(
		Pixel* blockPtr, byte pattern, Pixel fg, Pixel bg)
	{
		blockPtr[0] = pattern & 0x80 ? fg : bg;
		blockPtr[1] = pattern & 0x40 ? fg : bg;
		blockPtr[2] = pattern & 0x20 ? fg : bg;
		blockPtr[3] = pattern & 0x10 ? fg : bg;
		blockPtr[4] = pattern & 0x08 ? fg : bg;
		blockPtr[5] = pattern & 0x04 ? fg : bg;
		blockPtr[6] = pattern & 0x02 ? fg : bg;
		blockPtr[7] = pattern & 0x01 ? fg : bg;
	}

	/** Convert a monochrome 8*1 character block row to 8bpp alpha pixels.
	  * @param blockPtr Pointer to array where host pixels will be written to.
	  * @param pattern Pattern to convert.
	  */
	inline void convertMonoBlockRow(
		byte* blockPtr, const byte pattern)
	{
		blockPtr[0] = pattern & 0x80 ? 0xFF : 0;
		blockPtr[1] = pattern & 0x40 ? 0xFF : 0;
		blockPtr[2] = pattern & 0x20 ? 0xFF : 0;
		blockPtr[3] = pattern & 0x10 ? 0xFF : 0;
		blockPtr[4] = pattern & 0x08 ? 0xFF : 0;
		blockPtr[5] = pattern & 0x04 ? 0xFF : 0;
		blockPtr[6] = pattern & 0x02 ? 0xFF : 0;
		blockPtr[7] = pattern & 0x01 ? 0xFF : 0;
	}

	/** Convert a coloured 8*1 character block row to host pixels.
	  * @param blockPtr Pointer to array where host pixels will be written to.
	  * @param pattern Pattern to convert.
	  * @param fg Foreground colour.
	  * @param bg Background colour.
	  */
	inline void convertBlockRow(
		Pixel* blockPtr, byte pattern, byte colour)
	{
		convertBlockRow(
			blockPtr, pattern, palFg[colour >> 4], palFg[colour & 0x0F] );
	}

public:
	/** Convert a monochrome 8*8 character block to 8bpp alpha pixels.
	  * @param blockPtr Pointer to array where host pixels will be written to.
	  * @param patternPtr Pointer to 8 pattern entries to convert.
	  */
	inline void convertMonoBlock(
		byte* blockPtr, const byte* patternPtr)
	{
		convertMonoBlockRow(blockPtr,      patternPtr[0]);
		convertMonoBlockRow(blockPtr +  8, patternPtr[1]);
		convertMonoBlockRow(blockPtr + 16, patternPtr[2]);
		convertMonoBlockRow(blockPtr + 24, patternPtr[3]);
		convertMonoBlockRow(blockPtr + 32, patternPtr[4]);
		convertMonoBlockRow(blockPtr + 40, patternPtr[5]);
		convertMonoBlockRow(blockPtr + 48, patternPtr[6]);
		convertMonoBlockRow(blockPtr + 56, patternPtr[7]);
	}

	/** Convert a coloured 8*8 character block to host pixels.
	  * @param blockPtr Pointer to array where host pixels will be written to.
	  * @param patternPtr Pointer to 8 pattern entries to convert.
	  * @param colourPtr Pointer to 8 colour entries to convert.
	  */
	inline void convertColourBlock(
		Pixel* blockPtr, const byte* patternPtr, const byte* colourPtr)
	{
		convertBlockRow(blockPtr,      patternPtr[0], colourPtr[0]);
		convertBlockRow(blockPtr +  8, patternPtr[1], colourPtr[1]);
		convertBlockRow(blockPtr + 16, patternPtr[2], colourPtr[2]);
		convertBlockRow(blockPtr + 24, patternPtr[3], colourPtr[3]);
		convertBlockRow(blockPtr + 32, patternPtr[4], colourPtr[4]);
		convertBlockRow(blockPtr + 40, patternPtr[5], colourPtr[5]);
		convertBlockRow(blockPtr + 48, patternPtr[6], colourPtr[6]);
		convertBlockRow(blockPtr + 56, patternPtr[7], colourPtr[7]);
	}

	/** Select the display mode to use for scanline conversion.
	  * @param mode The new display mode.
	  */
	void setDisplayMode(DisplayMode mode);

private:
	typedef void (CharacterConverter::*RenderMethod)
		(Pixel* pixelPtr, int line);

	/** RenderMethods for each display mode.
	  */
	static RenderMethod modeToRenderMethod[];

	void renderText1   (Pixel* pixelPtr, int line);
	void renderText1Q  (Pixel* pixelPtr, int line);
	void renderText2   (Pixel* pixelPtr, int line);
	void renderGraphic1(Pixel* pixelPtr, int line);
	void renderGraphic2(Pixel* pixelPtr, int line);
	void renderMulti   (Pixel* pixelPtr, int line);
	void renderMultiQ  (Pixel* pixelPtr, int line);
	void renderBogus   (Pixel* pixelPtr, int line);
	void renderMultiHelper(Pixel* pixelPtr, int line,
	                       int mask, int patternQuarter);

	/** Rendering method for the current display mode.
	  */
	RenderMethod renderMethod;

	VDP& vdp;
	VDPVRAM& vram;

	const Pixel* palFg;
	const Pixel* palBg;

	/** Dirty tables indicate which character blocks must be repainted.
	  * The anyDirty variables are true when there is at least one
	  * element in the dirty table that is true.
	  * TODO:
	  * Change tracking is currently disabled, so every change variable
	  * is set to true. In the future change tracking will be restored.
	  */
	bool anyDirtyColour;
	std::bitset<(1<<10)> dirtyColour;
	bool anyDirtyPattern;
	std::bitset<(1<<10)> dirtyPattern;
	bool anyDirtyName;
	std::bitset<(1<<12)> dirtyName;
	bool dirtyForeground, dirtyBackground;
};

} // namespace openmsx

#endif
