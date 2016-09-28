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
//
//========================================================================

#include <config.h>

#ifdef USE_GCC_PRAGMAS
#pragma implementation
#endif

#include <string.h>
#include <stdio.h>

#include "goo/gmem.h"
#include "goo/GooString.h"
#include "goo/GooList.h"
#include "goo/GooHash.h"
#include "goo/gfile.h"
#include "Error.h"

#include "GlobalParams.h"
#include "GfxFont.h"

#if MULTITHREADED
#  define lockGlobalParams            gLockMutex(&mutex)
#  define lockUnicodeMapCache         gLockMutex(&unicodeMapCacheMutex)
#  define lockCMapCache               gLockMutex(&cMapCacheMutex)
#  define unlockGlobalParams          gUnlockMutex(&mutex)
#  define unlockUnicodeMapCache       gUnlockMutex(&unicodeMapCacheMutex)
#  define unlockCMapCache             gUnlockMutex(&cMapCacheMutex)
#else
#  define lockGlobalParams
#  define lockUnicodeMapCache
#  define lockCMapCache
#  define unlockGlobalParams
#  define unlockUnicodeMapCache
#  define unlockCMapCache
#endif

/* Mac implementation of external font matching code */

#include <ApplicationServices/ApplicationServices.h>

GBool GlobalParams::loadPlatformFont(const char * fontName) {

  CFStringRef psName = CFStringCreateWithCStringNoCopy(kCFAllocatorDefault,
							fontName,
							kCFStringEncodingASCII,
							kCFAllocatorNull);
  ATSFontRef fontRef = ATSFontFindFromPostScriptName(psName, kATSOptionFlagsDefault);
  CFRelease(psName);
  if (fontRef == kATSUInvalidFontID)
    return gFalse;

  // Currently support only TrueType fonts: check for presence of 'glyf' table
  // TODO: what about OpenType/CFF? DisplayFontParam doesn't seem to allow for this
#define TAG(a,b,c,d) (UInt32)((a)<<24) | (UInt32)((b)<<16) | (UInt32)((c)<<8) | (UInt32)(d)

  ByteCount tableSize;
  if (ATSFontGetTable(fontRef, TAG('g','l','y','f'), 0, 0, NULL, &tableSize) != noErr ||
      tableSize == 0)
    return gFalse;

  do { // if the font comes from a .ttf file, we can use that directly
    FSRef fsRef;
    if (ATSFontGetFileReference(fontRef, &fsRef) != noErr)
      break;

    UInt8 fontPath[PATH_MAX + 1];
    if (FSRefMakePath(&fsRef, fontPath, PATH_MAX) != noErr)
      break;

    int pathLen = strlen((const char *) fontPath);
    if (pathLen > 4 && fontPath[pathLen - 4] == '.') {
      const char * ext = (const char *) fontPath + pathLen - 3;

      // accept either .ttf or .otf extension; .otf could contain TrueType-format glyphs
      if (strcmp(ext, "ttf") == 0 || strcmp(ext, "TTF") == 0 ||
          strcmp(ext, "otf") == 0 || strcmp(ext, "OTF") == 0) {
        DisplayFontParam *dfp = new DisplayFontParam(new GooString(fontName), displayFontTT);
        dfp->setFileName(new GooString((const char *) fontPath));
        displayFonts->add(dfp->name, dfp);
        return gTrue;
      }
    }
  } while (0);

  // for .dfont or suitcase files, FoFiTrueType can't handle them, so we extract
  // the required font to a temporary .ttf file and then use that

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
    return gFalse;
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
    return gFalse;
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
    return gFalse;
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
    return gFalse;
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
    return gFalse;
  }

  void * p = realloc(tempFontFiles, (numTempFontFiles + 1) * sizeof(char *));
  if (!p) {
    unlink(fontPath);
    gfree(fontPath);
    return gFalse;
  }
  tempFontFiles = (char **) p;
  tempFontFiles[numTempFontFiles] = fontPath;
  ++numTempFontFiles;

  DisplayFontParam *dfp = new DisplayFontParam(new GooString(fontName), displayFontTT);
  dfp->setFileName(new GooString(fontPath));
  displayFonts->add(dfp->name, dfp);

  return gTrue;
}

static const char *
findSubstituteName(char * fontName) {
  GBool bold = (strstr(fontName, "Bold") != NULL ||
                strstr(fontName, "bold") != NULL || // to catch "Semibold", "Demibold", etc
                strstr(fontName, "Ultra") != NULL ||
                strstr(fontName, "Heavy") != NULL ||
                strstr(fontName, "Black") != NULL);
  GBool ital = (strstr(fontName, "Italic") != NULL ||
                strstr(fontName, "Oblique") != NULL);
  if (bold) {
    return ital ? "Helvetica-BoldOblique" : "Helvetica-Bold";
  } else {
    return ital ? "Helvetica-Oblique" : "Helvetica";
  }
}

DisplayFontParam *GlobalParams::getDisplayFont(GfxFont *font) {
  DisplayFontParam *  dfp;
  GooString *         fontName = font->getName();
  char *              substFontName = NULL;

  if (!fontName) return NULL;
  lockGlobalParams;

  dfp = (DisplayFontParam *)displayFonts->lookup(fontName);
  if (!dfp) {
    if (loadPlatformFont(fontName->getCString()))
      dfp = (DisplayFontParam *)displayFonts->lookup(fontName);
    if (!dfp) {
      substFontName = (char *) findSubstituteName(fontName->getCString());
      error(errIO, -1, "Couldn't find a font for '%s', subst is '%s'", fontName->getCString(), substFontName);
      dfp = (DisplayFontParam *)displayFonts->lookup(substFontName);
      if (!dfp) {
        if (loadPlatformFont(substFontName))
          dfp = (DisplayFontParam *)displayFonts->lookup(substFontName);
      }
    }
  }
  // this isn't supposed to fail, because the substitutes are system fonts
  // that should always be available
  assert(dfp);

  unlockGlobalParams;
  return dfp;
}
