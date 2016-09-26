//========================================================================
//
// GlobalParamsMac.cc
//
//========================================================================

//========================================================================
//
// Contributed to the Poppler project - http://poppler.freedesktop.org
//
// Copyright (c) 2009 Jonathan Kew
// Copyright (c) 2016 Henrik Grubbström <grubba@grubba.org>
//
//========================================================================

/* Mac implementation of external font matching code */

#include <ApplicationServices/ApplicationServices.h>

// Not needed for MacOS.
void GlobalParams::setupBaseFonts(char *dir) {
}

static GooString *findMacTTFFile(ATSFontRef fontRef,
				 SysFontType *type,
				 int *fontNum) {
  FSRef fsRef;
  if (ATSFontGetFileReference(fontRef, &fsRef) != noErr)
    return NULL;

  UInt8 fontPath[PATH_MAX + 1];
  if (FSRefMakePath(&fsRef, fontPath, PATH_MAX) != noErr)
    return NULL;

  int pathLen = strlen((const char *) fontPath);
  if (pathLen > 4 && fontPath[pathLen - 4] == '.') {
    const char * ext = (const char *) fontPath + pathLen - 3;

    // accept either .ttf or .otf extension; .otf could contain TrueType-format glyphs
    if (strcmp(ext, "ttf") == 0 || strcmp(ext, "TTF") == 0 ||
	strcmp(ext, "otf") == 0 || strcmp(ext, "OTF") == 0) {
      *type = sysFontTTF;
      *fontNum = 0;
      return new GooString((const char *)fontPath);
    }
  }
  return NULL;
}

GooString *GlobalParams::findSystemFontFile(GfxFont *font,
					    SysFontType *type,
					    int *fontNum) {
  SysFontInfo *fi = NULL;
  GooString *fontName = font->getName();
  const char *cfontName = fontName->getCString();
  GooString *path = NULL;

  if (!fontName) return NULL;
  fontName = fontName->copy();
  lockGlobalParams;

  if ((fi = sysFonts->find(fontName, gTrue))) {
    path = fi->path->copy();
    *type = fi->type;
    *fontNum = fi->fontNum;
  } else {
    CFStringRef psName = CFStringCreateWithCStringNoCopy(kCFAllocatorDefault,
							 cfontName,
							 kCFStringEncodingASCII,
							 kCFAllocatorNull);
    ATSFontRef fontRef = ATSFontFindFromPostScriptName(psName, kATSOptionFlagsDefault);
    CFRelease(psName);
    if (fontRef == kATSUInvalidFontID)
      goto done;

    // Currently support only TrueType fonts: check for presence of 'glyf' table
    // TODO: what about OpenType/CFF?
#define TAG(a,b,c,d) (UInt32)((a)<<24) | (UInt32)((b)<<16) | (UInt32)((c)<<8) | (UInt32)(d)

    ByteCount tableSize;
    if (ATSFontGetTable(fontRef, TAG('g','l','y','f'), 0, 0, NULL, &tableSize) != noErr ||
	tableSize == 0)
      goto done;

    // if the font comes from a .ttf file, we can use that directly
    path = findMacTTFFile(fontRef, type, fontNum);

    if (path) goto found_path;

    // for .dfont or suitcase files, FoFiTrueType can't handle them, so we
    // extract the required font to a temporary .ttf file and then use that

    struct sfntHeader {
      UInt32 version;
      UInt16 numTables;
      UInt16 searchRange;
      UInt16 entrySelector;
      UInt16 rangeShift;
      struct {
	UInt32 tag;
	UInt32 checkSum;
	UInt32 offset;
	UInt32 length;
      } dir[1];
    };

    ByteCount headerSize;
    if (ATSFontGetTableDirectory(fontRef, 0, NULL, &headerSize) != noErr)
      goto done;
    struct sfntHeader * header = (struct sfntHeader *) new Byte[headerSize];
    ATSFontGetTableDirectory(fontRef, headerSize, (Byte *) header, &headerSize);

#define READ16(x) (UInt16)(((UInt8*)&(x))[0]<<8) + (UInt16)((UInt8*)&(x))[1]
#define READ32(x) (UInt32)(((UInt8*)&(x))[0]<<24) + (UInt32)(((UInt8*)&(x))[1]<<16) + (UInt32)(((UInt8*)&(x))[2]<<8) + (UInt32)((UInt8*)&(x))[3]

    UInt32 version = READ32(header->version);
    if (version != 0x00010000 &&
	// TODO: figure out whether we can support OpenType/CFF fonts here
	//      version != TAG('O','T','T','0') &&
	version != TAG('t','r','u','e')) {
      delete [] (Byte *) header;
      goto done;
    }

    UInt16 numTables = READ16(header->numTables);
    UInt32 maxOffset = 0;
    for (UInt16 i = 0; i < numTables; ++i) {
      UInt32 end = READ32(header->dir[i].offset) + READ32(header->dir[i].length);
      if (end > maxOffset)
	maxOffset = end;
    }

    char * ttfData = new char[maxOffset];
    struct sfntHeader * newFont = (struct sfntHeader *) ttfData;

    newFont->version = header->version;

    UInt16 realTables = 0, tableIndex;
    for (tableIndex = 0; tableIndex < numTables; ++tableIndex) {
      ByteCount tableLoc = READ32(header->dir[tableIndex].offset);
      if (tableLoc == 0) // ATS synthetic table, do not copy
	continue;
      tableSize = READ32(header->dir[tableIndex].length);
      if (ATSFontGetTable(fontRef, READ32(header->dir[tableIndex].tag),
			  0, tableSize, ttfData + tableLoc, &tableSize) != noErr)
	break;
      newFont->dir[realTables] = header->dir[tableIndex];
      realTables++;
    }
    delete [] (Byte*) header;
    if (tableIndex < numTables) {
      delete [] ttfData;
      goto done;
    }

    newFont->numTables = READ16(realTables);
    UInt16 searchRange = realTables;
    searchRange |= searchRange >> 1;
    searchRange |= searchRange >> 2;
    searchRange |= searchRange >> 4;
    searchRange |= searchRange >> 8;
    searchRange &= ~searchRange >> 1;
    searchRange *= 16;
    newFont->searchRange = READ16(searchRange);
    UInt16 rangeShift = realTables * 16 - searchRange;
    UInt16 entrySelector = 0;
    while (searchRange > 16) {
      ++entrySelector;
      searchRange >>= 1;
    }
    newFont->entrySelector = READ16(entrySelector);
    newFont->rangeShift = READ16(rangeShift);

    char * fontPath = copyString("/tmp/XXXXXXXX.ttf");
    if (mkstemps(fontPath, 4) == -1) {
      delete [] ttfData;
      gfree(fontPath);
      goto done;
    }

    GBool writtenOk = gFalse;
    FILE * ttfFile = fopen(fontPath, "wb");
    if (ttfFile) {
      writtenOk = (fwrite(ttfData, 1, maxOffset, ttfFile) == maxOffset);
      fclose(ttfFile);
    }
    delete [] ttfData;
    if (!writtenOk) {
      unlink(fontPath);
      gfree(fontPath);
      goto done;
    }

    void * p = realloc(tempFontFiles, (numTempFontFiles + 1) * sizeof(char *));
    if (!p) {
      unlink(fontPath);
      gfree(fontPath);
      goto done;
    }
    tempFontFiles = (char **) p;
    tempFontFiles[numTempFontFiles] = fontPath;
    ++numTempFontFiles;

    *type = sysFontTTF;
    *fontNum = 0;
    path = new GooString(fontPath);
  }

 found_path:
  if (path) {
    GBool bold = (strstr(cfontName, "Bold") != NULL ||
		  strstr(cfontName, "bold") != NULL || // to catch "Semibold", "Demibold", etc
		  strstr(cfontName, "Ultra") != NULL ||
		  strstr(cfontName, "Heavy") != NULL ||
		  strstr(cfontName, "Black") != NULL);
    GBool ital = (strstr(cfontName, "Italic") != NULL ||
		  strstr(cfontName, "Oblique") != NULL);

    fi = new SysFontInfo(fontName->copy(), bold, italic,
			 path->copy(), *type, *fontNum);
    sysFonts->addMacFont(fi);
  } else if ((fi = sysFonts->find(fontName, gFalse))) {
    path = fi->path->copy();
    *type = fi->type;
    *fontNum = fi->fontNum;
  }
 done:
  unlockGlobalParams;
  return path;
}
