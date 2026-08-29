// Burner cheat-file loader
#include "burner.h"
#include "neocdlist.h"

//  New-style text loading: read entire file into memory, auto-detect encoding,
//  convert to TCHAR text in-memory (no CRT ccs mode, no in-place disk rewriting).

enum { CONC_ENC_ANSI, CONC_ENC_UTF8, CONC_ENC_UTF8_BOM, CONC_ENC_UTF16_LE, CONC_ENC_UTF16_BE };

static bool conc_is_valid_utf8(const unsigned char* buf, size_t len)
{
	size_t p = 0;
	while (p < len) {
		unsigned char c = buf[p];
		if (c < 0x80) { p++; continue; }
		int nExtra;
		if      (c >= 0xC2 && c <= 0xDF) nExtra = 1;
		else if (c >= 0xE0 && c <= 0xEF) nExtra = 2;
		else if (c >= 0xF0 && c <= 0xF4) nExtra = 3;
		else return false;
		for (int i = 1; i <= nExtra; i++) {
			if (p + (size_t)i >= len) return false;
			if (0x80 != (buf[p + i] & 0xC0)) return false;
		}
		p += nExtra + 1;
	}
	return true;
}

static int conc_detect_encoding(const unsigned char* buf, size_t len)
{
	if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)	return CONC_ENC_UTF8_BOM;
	if (len >= 2 && buf[0] == 0xFF && buf[1] == 0xFE)					return CONC_ENC_UTF16_LE;
	if (len >= 2 && buf[0] == 0xFE && buf[1] == 0xFF)					return CONC_ENC_UTF16_BE;
	if (len == 0)														return CONC_ENC_ANSI;
	size_t scan = len < 4096 ? len : 4096;
	size_t zeroEven = 0, zeroOdd = 0;
	for (size_t i = 0; i < scan; i++) {
		if (buf[i] == 0x00) { if (i & 1) zeroOdd++; else zeroEven++; }
	}
	if ((zeroEven + zeroOdd) * 4 >= scan) {
		if (zeroOdd  > zeroEven * 4) return CONC_ENC_UTF16_LE;
		if (zeroEven > zeroOdd  * 4) return CONC_ENC_UTF16_BE;
	}
	return conc_is_valid_utf8(buf, len) ? CONC_ENC_UTF8 : CONC_ENC_ANSI;
}

// Read entire file into a malloc'd TCHAR buffer (UTF-8→TCHAR on non-Win32;
// UTF-8→UTF-16 wchar_t on Win32 _UNICODE).  Caller must free().
static TCHAR* conc_load_text(const TCHAR* pszFileName)
{
	FILE* fp = _tfopen(pszFileName, _T("rb"));
	if (!fp) return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
	long sz = ftell(fp);
	if (sz < 0) { fclose(fp); return NULL; }
	rewind(fp);
	unsigned char* raw = (unsigned char*)malloc((size_t)sz + 1);
	if (!raw) { fclose(fp); return NULL; }
	size_t got = fread(raw, 1, (size_t)sz, fp);
	raw[got] = 0;
	fclose(fp);

	int enc = conc_detect_encoding(raw, got);

	// Step 1: decode raw bytes to UTF-8 (malloc'd, caller-freed path returns this later on non-Win32)
	char* u8;
	if (enc == CONC_ENC_UTF8 || enc == CONC_ENC_ANSI) {
#ifdef BUILD_WIN32
		if (enc == CONC_ENC_ANSI) {
			INT32 wn = MultiByteToWideChar(CP_ACP, 0, (char*)raw, (INT32)got, NULL, 0);
			if (wn > 0) {
				wchar_t* w = (wchar_t*)malloc((size_t)(wn + 1) * sizeof(wchar_t));
				if (w) {
					MultiByteToWideChar(CP_ACP, 0, (char*)raw, (INT32)got, w, wn);
					w[wn] = 0;
					free(raw);
					return (TCHAR*)w;
				}
			}
		}
#endif
		u8 = (char*)raw;
	} else if (enc == CONC_ENC_UTF8_BOM) {
		memmove(raw, raw + 3, got - 3 + 1);
		u8 = (char*)raw;
	} else {
		// UTF-16 LE/BE → UTF-8
		size_t start = 2;
		size_t units = (got - start) / 2;
		u8 = (char*)malloc(units * 3 + 1);
		if (!u8) { free(raw); return NULL; }
		size_t o = 0;
		for (size_t i = 0; i < units; i++) {
			unsigned int cp;
			unsigned char b0 = raw[start + i*2 + 0];
			unsigned char b1 = raw[start + i*2 + 1];
			cp = (enc == CONC_ENC_UTF16_LE) ? (unsigned int)(b0 | (b1 << 8))
			                               : (unsigned int)(b1 | (b0 << 8));
			if (cp < 0x80) {
				u8[o++] = (char)cp;
			} else if (cp < 0x800) {
				u8[o++] = (char)(0xC0 | (cp >> 6));
				u8[o++] = (char)(0x80 | (cp & 0x3F));
			} else {
				u8[o++] = (char)(0xE0 | (cp >> 12));
				u8[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
				u8[o++] = (char)(0x80 | (cp & 0x3F));
			}
		}
		u8[o] = 0;
		free(raw);
	}

#ifdef _UNICODE
	// Win32 TCHAR = wchar_t (UTF-16): convert UTF-8 to wchar_t
	INT32 wn = MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0);
	if (wn <= 0) { free(u8); return NULL; }
	wchar_t* w = (wchar_t*)malloc((size_t)wn * sizeof(wchar_t));
	if (!w) { free(u8); return NULL; }
	MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, wn);
	free(u8);
	return (TCHAR*)w;
#else
	// Non-Win32 TCHAR = char: UTF-8 is directly usable
	return (TCHAR*)u8;
#endif
}

// In-memory replacement for _fgetts: copies next line (up to \n inclusive)
// into szLine, null-terminates.  *ppSave is advanced.  Returns szLine or NULL at EOF.
static TCHAR* conc_gets(TCHAR* szLine, INT32 nMaxLen, const TCHAR* pText, TCHAR** ppSave)
{
	TCHAR* p = *ppSave ? *ppSave : (TCHAR*)pText;
	if (!p || *p == _T('\0')) return NULL;
	INT32 i = 0;
	while (i < nMaxLen - 1 && *p && *p != _T('\n')) {
		szLine[i++] = *p++;
	}
	if (*p == _T('\n') && i < nMaxLen - 1) {
		szLine[i++] = *p++;
	}
	szLine[i] = _T('\0');
	*ppSave = p;
	return szLine;
}

// GameGenie stuff is handled a little differently..
#define HW_NES ( ((BurnDrvGetHardwareCode() & HARDWARE_PUBLIC_MASK) == HARDWARE_NES) || ((BurnDrvGetHardwareCode() & HARDWARE_PUBLIC_MASK) == HARDWARE_FDS) )
#define HW_SNES ( ((BurnDrvGetHardwareCode() & HARDWARE_PUBLIC_MASK) == HARDWARE_SNES) )
#define HW_GGENIE ( HW_NES || HW_SNES )

static CheatInfo* pCurrentCheat = NULL;
static CheatInfo* pPreviousCheat = NULL;

static bool SkipComma(TCHAR** s)
{
	while (**s && **s != _T(',')) {
		(*s)++;
	}

	if (**s == _T(',')) {
		(*s)++;
	}

	if (**s) {
		return true;
	}

	return false;
}

static void CheatError(TCHAR* pszFilename, INT32 nLineNumber, CheatInfo* pCheat, TCHAR* pszInfo, TCHAR* pszLine)
{
#if defined (BUILD_WIN32)
	FBAPopupAddText(PUF_TEXT_NO_TRANSLATE, _T("Cheat file %s is malformed.\nPlease remove or repair the file.\n\n"), pszFilename);
	if (pCheat) {
		FBAPopupAddText(PUF_TEXT_NO_TRANSLATE, _T("Parse error at line %i, in cheat \"%s\".\n"), nLineNumber, pCheat->szCheatName);
	} else {
		FBAPopupAddText(PUF_TEXT_NO_TRANSLATE, _T("Parse error at line %i.\n"), nLineNumber);
	}

	if (pszInfo) {
		FBAPopupAddText(PUF_TEXT_NO_TRANSLATE, _T("Problem:\t%s.\n"), pszInfo);
	}
	if (pszLine) {
		FBAPopupAddText(PUF_TEXT_NO_TRANSLATE, _T("Text:\t%s\n"), pszLine);
	}

	FBAPopupDisplay(PUF_TYPE_ERROR);
#endif

#if defined(BUILD_SDL2)
	printf("Cheat file %s is malformed.\nPlease remove or repair the file.\n\n", pszFilename);
	if (pCheat) {
		printf("Parse error at line %i, in cheat \"%s\".\n", nLineNumber, pCheat->szCheatName);
	} else {
		printf("Parse error at line %i.\n", nLineNumber);
	}

	if (pszInfo) {
		printf("Problem:\t%s.\n", pszInfo);
	}
	if (pszLine) {
		printf("Text:\t%s\n", pszLine);
	}
#endif
}

static TCHAR* getFilenameFromPath(TCHAR* path) {
	if (!path) {
		return NULL;
	}

    TCHAR* filename = path;

    for (TCHAR* p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            filename = p + 1;
        }
    }
    return filename;
}

static void CheatLinkNewNode(TCHAR *szDerp)
{
	// Link new node into the list
	pPreviousCheat = pCurrentCheat;
	pCurrentCheat = (CheatInfo*)malloc(sizeof(CheatInfo));
	if (pCheatInfo == NULL) {
		pCheatInfo = pCurrentCheat;
	}

	memset(pCurrentCheat, 0, sizeof(CheatInfo));
	pCurrentCheat->pPrevious = pPreviousCheat;
	if (pPreviousCheat) {
		pPreviousCheat->pNext = pCurrentCheat;
	}

	// Fill in defaults
	pCurrentCheat->nType = 0;							    // Default to cheat type 0 (apply each frame)
	pCurrentCheat->nStatus = -1;							// Disable cheat
	pCurrentCheat->nDefault = 0;							// Set default option
	pCurrentCheat->bOneShot = 0;							// Set default option (off)
	pCurrentCheat->bWatchMode = 0;							// Set default option (off)

	_tcsncpy (pCurrentCheat->szCheatName, szDerp, QUOTE_MAX);
}

static INT32 ConfigParseFile(TCHAR* pszFilename)
{
#define INSIDE_NOTHING (0xFFFF & (1 << ((sizeof(TCHAR) * 8) - 1)))

	TCHAR szLine[8192];
	TCHAR* s;
	TCHAR* t;
	INT32 nLen;
	bool bFirst = true;

	INT32 nLine = 0;
	TCHAR nInside = INSIDE_NOTHING;

	TCHAR* text = conc_load_text(pszFilename);
	TCHAR* pszFileHeading = getFilenameFromPath(pszFilename);
	if (text == NULL) {
		if ((BurnDrvGetFlags() & BDF_CLONE) && BurnDrvGetText(DRV_PARENT)) {
			TCHAR szAlternative[MAX_PATH] = { 0 };
			_stprintf(szAlternative, _T("%s%s.ini"), szAppCheatsPath, BurnDrvGetText(DRV_PARENT));

			text = conc_load_text(szAlternative);
			if (text == NULL)
				return 1;
			pszFileHeading = getFilenameFromPath(szAlternative);
		} else {
			return 1;
		}
	}

	TCHAR* save = NULL;
	while (conc_gets(szLine, 8192, text, &save) != NULL) {

		nLine++;

		nLen = _tcslen(szLine);

		// Get rid of the linefeed at the end
		while ((nLen > 0) && (szLine[nLen - 1] == 0x0A || szLine[nLen - 1] == 0x0D)) {
			szLine[nLen - 1] = 0;
			nLen--;
		}

		s = szLine;													// Start parsing

		if (s[0] == _T('/') && s[1] == _T('/')) {					// Comment
			continue;
		}

		if ((t = LabelCheck(s, _T("include"))) != 0) {				// Include a file
			s = t;

			TCHAR szFilename[MAX_PATH] = _T("");

			// Read name of the cheat file
			TCHAR* szQuote = NULL;
			QuoteRead(&szQuote, NULL, s);

			_stprintf(szFilename, _T("%s%s.dat"), szAppCheatsPath, szQuote);

			if (ConfigParseFile(szFilename)) {
				_stprintf(szFilename, _T("%s%s.ini"), szAppCheatsPath, szQuote);
				if (ConfigParseFile(szFilename)) {
					CheatError(pszFilename, nLine, NULL, _T("included file doesn't exist"), szLine);
				}
			}

			continue;
		}

		if ((t = LabelCheck(s, _T("cheat"))) != 0) {				// Add new cheat
			s = t;

			// Read cheat name
			TCHAR* szQuote = NULL;
			TCHAR* szEnd = NULL;

			QuoteRead(&szQuote, &szEnd, s);

			s = szEnd;

			if ((t = LabelCheck(s, _T("advanced"))) != 0) {			// Advanced cheat
				s = t;
			}

			SKIP_WS(s);

			if (nInside == _T('{')) {
				CheatError(pszFilename, nLine, pCurrentCheat, _T("missing closing bracket"), NULL);
				break;
			}
#if 0
			if (*s != _T('\0') && *s != _T('{')) {
				CheatError(pszFilename, nLine, NULL, _T("malformed cheat declaration"), szLine);
				break;
			}
#endif
			nInside = *s;

#ifndef __LIBRETRO__
			if (bFirst) {
				TCHAR szHeading[256];
				_stprintf(szHeading, _T("[ Cheats \"%s\" ]"), pszFileHeading);
				CheatLinkNewNode(szHeading);
				bFirst = false;
			}
#endif

			CheatLinkNewNode(szQuote);

			continue;
		}

#ifdef __LIBRETRO__
		_tcsncpy (pCurrentCheat->szCheatFilename, pszFileHeading, QUOTE_MAX);
#endif

		if ((t = LabelCheck(s, _T("type"))) != 0) {					// Cheat type
			if (nInside == INSIDE_NOTHING || pCurrentCheat == NULL) {
				CheatError(pszFilename, nLine, pCurrentCheat, _T("rogue cheat type"), szLine);
				break;
			}
			s = t;

			// Set type
			pCurrentCheat->nType = _tcstol(s, NULL, 0);

			continue;
		}

		if ((t = LabelCheck(s, _T("default"))) != 0) {				// Default option
			if (nInside == INSIDE_NOTHING || pCurrentCheat == NULL) {
				CheatError(pszFilename, nLine, pCurrentCheat, _T("rogue default"), szLine);
				break;
			}
			s = t;

			// Set default option
			pCurrentCheat->nDefault = _tcstol(s, NULL, 0);

			continue;
		}

		INT32 n = _tcstol(s, &t, 0);
		if (t != s) {				   								// New option

			if (nInside == INSIDE_NOTHING || pCurrentCheat == NULL) {
				CheatError(pszFilename, nLine, pCurrentCheat, _T("rogue option"), szLine);
				break;
			}

			// Link a new Option structure to the cheat
			if (n < CHEAT_MAX_OPTIONS) {
				s = t;

				// Read option name
				TCHAR* szQuote = NULL;
				TCHAR* szEnd = NULL;
				if (QuoteRead(&szQuote, &szEnd, s)) {
					CheatError(pszFilename, nLine, pCurrentCheat, _T("option name omitted"), szLine);
					break;
				}
				s = szEnd;

				if (pCurrentCheat->pOption[n] == NULL) {
					pCurrentCheat->pOption[n] = (CheatOption*)malloc(sizeof(CheatOption));
				}
				memset(pCurrentCheat->pOption[n], 0, sizeof(CheatOption));

				memcpy(pCurrentCheat->pOption[n]->szOptionName, szQuote, QUOTE_MAX * sizeof(TCHAR));

				INT32 nCurrentAddress = 0;
				bool bOK = true;
				while (nCurrentAddress < CHEAT_MAX_ADDRESS) {
					INT32 nCPU = 0, nAddress = 0, nValue = 0;

					if (SkipComma(&s)) {
						if (HW_GGENIE) {
							t = s;
							INT32 newlen = 0;
#if defined(BUILD_WIN32)
							for (INT32 z = 0; z < lstrlen(t); z++) {
#else
							for (INT32 z = 0; z < strlen(t); z++) {
#endif
								char c = toupper((char)*s);
								if ( ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c == '-' || c == ':')) && newlen < 10)
									pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].szGenieCode[newlen++] = c;
								s++;
								if (*s == _T(',')) break;
							}
							nAddress = 0xffff; // nAddress not used, but needs to be nonzero (NES/Game Genie)
						} else {
							nCPU = _tcstol(s, &t, 0);		// CPU number
							if (t == s) {
								CheatError(pszFilename, nLine, pCurrentCheat, _T("CPU number omitted"), szLine);
								bOK = false;
								break;
							}
							s = t;

							SkipComma(&s);
							nAddress = _tcstol(s, &t, 0);	// Address
							if (t == s) {
								bOK = false;
								CheatError(pszFilename, nLine, pCurrentCheat, _T("address omitted"), szLine);
								break;
							}
							s = t;

							SkipComma(&s);
							nValue = _tcstol(s, &t, 0);		// Value
							if (t == s) {
								bOK = false;
								CheatError(pszFilename, nLine, pCurrentCheat, _T("value omitted"), szLine);
								break;
							}
						}
					} else {
						if (nCurrentAddress) {			// Only the first option is allowed no address
							break;
						}
						if (n) {
							bOK = false;
							CheatError(pszFilename, nLine, pCurrentCheat, _T("CPU / address / value omitted"), szLine);
							break;
						}
					}

					pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nCPU = nCPU;
					pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nAddress = nAddress;
					pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nValue = nValue;
					nCurrentAddress++;
				}

				if (!bOK) {
					break;
				}

			}

			continue;
		}

		SKIP_WS(s);
		if (*s == _T('}')) {
			if (nInside != _T('{')) {
				CheatError(pszFilename, nLine, pCurrentCheat, _T("missing opening bracket"), NULL);
				break;
			}

			nInside = INSIDE_NOTHING;
		}

		// Line isn't (part of) a valid cheat
#if 0
		if (*s) {
			CheatError(pszFilename, nLine, NULL, _T("rogue line"), szLine);
			break;
		}
#endif

	}

	free(text);

	return 0;
}


//TODO: make cross platform
static INT32 ConfigParseNebulaFile(TCHAR* pszFilename)
{
	TCHAR* text = conc_load_text(pszFilename);
	TCHAR* pszFileHeading = getFilenameFromPath(pszFilename);
	if (text == NULL) {
		if ((BurnDrvGetFlags() & BDF_CLONE) && BurnDrvGetText(DRV_PARENT)) {
			TCHAR szAlternative[MAX_PATH] = { 0 };
			_stprintf(szAlternative, _T("%s%s.dat"), szAppCheatsPath, BurnDrvGetText(DRV_PARENT));

			text = conc_load_text(szAlternative);
			if (text == NULL)
				return 1;
			pszFileHeading = getFilenameFromPath(szAlternative);
		} else {
			return 1;
		}
	}

	INT32 nLen;
	INT32 i, j, n = 0;
	TCHAR tmp[32];
	TCHAR szLine[1024];
	bool bFirst = true;

	TCHAR* save = NULL;
	while (conc_gets(szLine, 1024, text, &save) != NULL)
	{
		nLen = _tcslen(szLine);

		while ((nLen > 0) && (szLine[nLen - 1] == 0x0A || szLine[nLen - 1] == 0x0D)) {
			szLine[nLen - 1] = 0;
			nLen--;
		}

		if (nLen < 3 || szLine[0] == '[') continue;

		if (!_tcsncmp (_T("Name="), szLine, 5))
		{
			n = 0;

#ifndef __LIBRETRO__
			if (bFirst) {
				TCHAR szHeading[256];
				_stprintf(szHeading, _T("[ Cheats \"%s\" (Nebula) ]"), pszFileHeading);
				CheatLinkNewNode(szHeading);
				bFirst = false;
			}
#endif

			CheatLinkNewNode(szLine + 5);

			pCurrentCheat->szCheatName[nLen-6] = '\0';

			continue;
		}

#ifdef __LIBRETRO__
		_tcsncpy (pCurrentCheat->szCheatFilename, pszFileHeading, QUOTE_MAX);
#endif

		if (!_tcsncmp (_T("Default="), szLine, 8) && n >= 0)
		{
			_tcsncpy (tmp, szLine + 8, nLen-9);
			tmp[nLen-9] = '\0';
#if defined(BUILD_WIN32)
			_stscanf (tmp, _T("%d"), &(pCurrentCheat->nDefault));
#else
			sscanf (tmp, _T("%d"), &(pCurrentCheat->nDefault));
#endif
			continue;
		}


		i = 0, j = 0;
		while (i < nLen)
		{
			if (szLine[i] == '=' && i < 4) j = i+1;
			if (szLine[i] == ',' || szLine[i] == '\r' || szLine[i] == '\n')
			{
				if (pCurrentCheat->pOption[n] == NULL) {
					pCurrentCheat->pOption[n] = (CheatOption*)malloc(sizeof(CheatOption));
				}
				memset(pCurrentCheat->pOption[n], 0, sizeof(CheatOption));

				_tcsncpy (pCurrentCheat->pOption[n]->szOptionName, szLine + j, QUOTE_MAX * sizeof(TCHAR));
				pCurrentCheat->pOption[n]->szOptionName[i-j] = '\0';

				i++; j = i;
				break;
			}
			i++;
		}

		INT32 nAddress = -1, nValue = 0, nCurrentAddress = 0;
		while (nCurrentAddress < CHEAT_MAX_ADDRESS)
		{
			if (i == nLen) break;

			if (szLine[i] == ',' || szLine[i] == '\r' || szLine[i] == '\n')
			{
				_tcsncpy (tmp, szLine + j, i-j);
				tmp[i-j] = '\0';

				if (nAddress == -1) {
#if defined(BUILD_WIN32)
					_stscanf (tmp, _T("%x"), &nAddress);
#else
					sscanf (tmp, _T("%x"), &nAddress);
#endif
				} else {
#if defined(BUILD_WIN32)
					_stscanf (tmp, _T("%x"), &nValue);
#else
					sscanf (tmp, _T("%x"), &nValue);
#endif

					pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nCPU = 0; 	// Always
					pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nAddress = nAddress ^ 1;
					pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nValue = nValue;
					nCurrentAddress++;

					nAddress = -1;
					nValue = 0;
				}
				j = i+1;
			}
			i++;
		}
		n++;
	}

	free(text);

	return 0;
}

#define IS_MIDWAY ((BurnDrvGetHardwareCode() & HARDWARE_PREFIX_MIDWAY) == HARDWARE_PREFIX_MIDWAY)

static INT32 ConfigParseMAMEFile_internal(const TCHAR *pText, const TCHAR *pszFileHeading, const TCHAR *name)
{
#define AddressInfo()	\
	INT32 k = (flags >> 20) & 3;	\
	INT32 cpu = (flags >> 24) & 0x1f; \
	if (cpu > 3) cpu = 0; \
	for (INT32 i = 0; i < k+1; i++) {	\
		pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nCPU = cpu;	\
		if ((flags & 0xf0000000) == 0x80000000) { \
			pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].bRelAddress = 1; \
			pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nRelAddressOffset = nAttrib; \
			pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nRelAddressBits = (flags & 0x3000000) >> 24; \
		} \
		pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nAddress = (pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].bRelAddress) ? nAddress : nAddress + i;	\
		pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nExtended = nAttrib; \
		pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nValue = (nValue >> ((k*8)-(i*8))) & 0xff;	\
		pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nMask = (nAttrib >> ((k*8)-(i*8))) & 0xff;	\
		pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nMultiByte = i;	\
		pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nTotalByte = k+1;	\
		nCurrentAddress++;	\
	}	\

#define AddressInfoGameGenie() { \
		pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nTotalByte = 1;	\
		pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nAddress = 0xffff; \
		strcpy(pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].szGenieCode, szGGenie); \
		nCurrentAddress++;	\
	}

#define OptionName(a)	\
	if (pCurrentCheat->pOption[n] == NULL) {						\
		pCurrentCheat->pOption[n] = (CheatOption*)malloc(sizeof(CheatOption));		\
	}											\
	memset(pCurrentCheat->pOption[n], 0, sizeof(CheatOption));				\
	_tcsncpy (pCurrentCheat->pOption[n]->szOptionName, a, QUOTE_MAX * sizeof(TCHAR));	\

#define tmpcpy(a)	\
	_tcsncpy (tmp, szLine + c0[a] + 1, c0[a+1] - (c0[a]+1));	\
	tmp[c0[a+1] - (c0[a]+1)] = '\0';				\

	TCHAR tmp[256];
	TCHAR tmp2[256];
	TCHAR gName[64];
	TCHAR szLine[1024];
	char szGGenie[128] = { 0, };

	INT32 nLen;
	INT32 n = 0;
	INT32 menu = 0;
	INT32 nFound = 0;
	INT32 nCurrentAddress = 0;
	UINT32 flags = 0;
	UINT32 nAddress = 0;
	UINT32 nValue = 0;
	UINT32 nAttrib = 0;
	bool bFirst = true;

	_stprintf(gName, _T(":%s:"), name);

	TCHAR* save = NULL;
	while (conc_gets(szLine, 1024, pText, &save) != NULL)
	{
		nLen = _tcslen (szLine);

		if (szLine[0] == ';') continue;

		/*
		 // find the cheat flags & 0x80000000 cheats (for debugging) -dink
		 int derpy = 0;
		 for (INT32 i = 0; i < nLen; i++) {
		 	if (szLine[i] == ':') {
		 		derpy++;
		 		if (derpy == 2 && szLine[i+1] == '8') {
					bprintf(0, _T("%s\n"), szLine);
				}
			}
		}
		*/

#if defined(BUILD_WIN32)
		if (_tcsncmp (szLine, gName, lstrlen(gName))) {
#else
		if (_tcsncmp (szLine, gName, strlen(gName))) {
#endif
			if (nFound) break;
			else continue;
		}

		if (_tcsstr(szLine, _T("----:REASON"))) {
			// reason to leave!
			break;
		}

		nFound = 1;

		INT32 c0[16], c1 = 0;					// find colons / break
		for (INT32 i = 0; i < nLen; i++)
			if (szLine[i] == ':' || szLine[i] == '\r' || szLine[i] == '\n')
				c0[c1++] = i;

		tmpcpy(1);						// control flags
#if defined(BUILD_WIN32)
		_stscanf (tmp, _T("%x"), &flags);
#else
		sscanf (tmp, _T("%x"), &flags);
#endif

		tmpcpy(2);						// cheat address
#if defined(BUILD_WIN32)
		_stscanf (tmp, _T("%x"), &nAddress);
		strcpy(szGGenie, TCHARToANSI(tmp, NULL, 0));
#else
		sscanf (tmp, _T("%x"), &nAddress);
		strcpy(szGGenie, tmp);
#endif

		tmpcpy(3);						// cheat value
#if defined(BUILD_WIN32)
		_stscanf (tmp, _T("%x"), &nValue);
#else
		sscanf (tmp, _T("%x"), &nValue);
#endif

		tmpcpy(4);						// cheat attribute
#if defined(BUILD_WIN32)
		_stscanf (tmp, _T("%x"), &nAttrib);
#else
		sscanf (tmp, _T("%x"), &nAttrib);
#endif

		tmpcpy(5);						// cheat name

		// & 0x4000 = don't add to list
		// & 0x0800 = BCD
		if (flags & 0x00004800) continue;			// skip various cheats (unhandled methods at this time)

		if ((flags & 0xff000000) == 0x39000000 && IS_MIDWAY) {
			nAddress |= 0xff800000 >> 3; // 0x39 = address is relative to system's ROM block, only midway uses this kinda cheats
		}

		if ( flags & 0x00008000 || (flags & 0x00010000 && !menu)) { // Linked cheat "(2/2) etc.."
			if (nCurrentAddress < CHEAT_MAX_ADDRESS) {
				if (HW_GGENIE) {
					AddressInfoGameGenie();
				} else {
					AddressInfo();
				}
			}

			continue;
		}

		if (~flags & 0x00010000) {
			n = 0;
			menu = 0;
			nCurrentAddress = 0;

#ifndef __LIBRETRO__
			if (bFirst) {
				TCHAR szHeading[256];
				_stprintf(szHeading, _T("[ Cheats \"%s\" ]"), pszFileHeading);
				CheatLinkNewNode(szHeading);
				bFirst = false;
			}
#endif

			CheatLinkNewNode(tmp);

#ifdef __LIBRETRO__
			_tcsncpy (pCurrentCheat->szCheatFilename, pszFileHeading, QUOTE_MAX);
#endif

#if defined(BUILD_WIN32)
			if (lstrlen(tmp) <= 0 || flags == 0x60000000) {
#else
			if (strlen(tmp) <= 0 || flags == 0x60000000) {
#endif
				n++;
				continue;
			}

			OptionName(_T("Disabled"));

			if (nAddress || HW_GGENIE) {
				if ((flags & 0x80018) == 0 && nAttrib != 0xffffffff) {
					pCurrentCheat->bWriteWithMask = 1; // nAttrib field is the mask
				}
				if (flags & 0x1) {
					pCurrentCheat->bOneShot = 1; // apply once and stop
				}
				if (flags & 0x2) {
					pCurrentCheat->bWaitForModification = 1; // wait for modification before changing
				}
				if (flags & 0x80000) {
					pCurrentCheat->bWaitForModification = 2; // check address against extended field before changing
				}
				if (flags & 0x800000) {
					pCurrentCheat->bRestoreOnDisable = 1; // restore previous value on disable
				}
				if (flags & 0x3000) {
					pCurrentCheat->nPrefillMode = (flags & 0x3000) >> 12;
				}
				if ((flags & 0x6) == 0x6) {
					pCurrentCheat->bWatchMode = 1; // display value @ address
				}
				if (flags & 0x100) { // add options
					INT32 nTotal = nValue + 1;
					INT32 nPlus1 = (flags & 0x200) ? 1 : 0; // displayed value +1?
					INT32 nStartValue = (flags & 0x400) ? 1 : 0; // starting value

					//bprintf(0, _T("adding .. %X. options\n"), nTotal);
					if (nTotal > 0xff) continue; // bad entry (roughrac has this)
					for (nValue = nStartValue; nValue < nTotal; nValue++) {
#if defined(UNICODE)
						swprintf(tmp2, L"# %d.", nValue + nPlus1);
#else
						sprintf(tmp2, _T("# %d."), nValue + nPlus1);
#endif
						n++;
						nCurrentAddress = 0;
						OptionName(tmp2);
						if (HW_GGENIE) {
							AddressInfoGameGenie();
						} else {
							AddressInfo();
						}
					}
				} else {
					n++;
					OptionName(tmp);
					if (HW_GGENIE) {
						AddressInfoGameGenie();
					} else {
						AddressInfo();
					}
				}
			} else {
				menu = 1;
			}

			continue;
		}

		if ( flags & 0x00010000 && menu) {
			n++;
			nCurrentAddress = 0;

			if ((flags & 0x80018) == 0 && nAttrib != 0xffffffff) {
				pCurrentCheat->bWriteWithMask = 1; // nAttrib field is the mask
			}
			if (flags & 0x1) {
				pCurrentCheat->bOneShot = 1; // apply once and stop
			}
			if (flags & 0x2) {
				pCurrentCheat->bWaitForModification = 1; // wait for modification before changing
			}
			if (flags & 0x80000) {
				pCurrentCheat->bWaitForModification = 2; // check address against extended field before changing
			}
			if (flags & 0x800000) {
				pCurrentCheat->bRestoreOnDisable = 1; // restore previous value on disable
			}
			if (flags & 0x3000) {
				pCurrentCheat->nPrefillMode = (flags & 0x3000) >> 12;
			}
			if ((flags & 0x6) == 0x6) {
				pCurrentCheat->bWatchMode = 1; // display value @ address
			}

			OptionName(tmp);
			if (HW_GGENIE) {
				AddressInfoGameGenie();
			} else {
				AddressInfo();
			}

			continue;
		}
	}

	// if no cheat was found, don't return success code
	if (pCurrentCheat == NULL) return 1;

	return 0;
}

static INT32 ConfigParseMAMEFile(int is_wayder)
{
	TCHAR szFileName[MAX_PATH] = _T("");

	if (is_wayder) {
		if (HW_NES || HW_SNES) return 1;
		_stprintf(szFileName, _T("%swayder_cheat.dat"), szAppCheatsPath);
	} else {
		if (HW_NES) {
			_stprintf(szFileName, _T("%scheatnes.dat"), szAppCheatsPath);
		} else if (HW_SNES) {
			_stprintf(szFileName, _T("%scheatsnes.dat"), szAppCheatsPath);
		} else {
			_stprintf(szFileName, _T("%scheat.dat"), szAppCheatsPath);
		}
	}

	TCHAR* text = conc_load_text(szFileName);
	TCHAR* pszFileHeading = getFilenameFromPath(szFileName);

	INT32 ret = 1;

	if (text) {
		ret = ConfigParseMAMEFile_internal(text, pszFileHeading, BurnDrvGetText(DRV_NAME));
		// try parent entry as fallback
		if (ret && (BurnDrvGetFlags() & BDF_CLONE) && BurnDrvGetText(DRV_PARENT)) {
			ret = ConfigParseMAMEFile_internal(text, pszFileHeading, BurnDrvGetText(DRV_PARENT));
		}

		free(text);
	}

	return ret;
}

static int encodeNES(int address, int value, int compare, char *result) {
	const char ALPHABET_NES[2][16+1] = { { "APZLGITYEOXUKSVN" }, { "apzlgityeoxuksvn" } };

	bool address_lower = !(address & 0x8000);

	unsigned int genie = ((value & 0x80) >> 4) | (value & 0x7);
    unsigned int temp = ((address & 0x80) >> 4) | ((value & 0x70) >> 4);
	genie <<= 4;
	genie |= temp;

	temp = (address & 0x70) >> 4;

	if (compare != -1) {
		temp |= 0x8;
	}

	genie <<= 4;
	genie |= temp;

	temp = (address & 0x8) | ((address & 0x7000) >> 12);
	genie <<= 4;
	genie |= temp;

	temp = ((address & 0x800) >> 8) | (address & 0x7);
	genie <<= 4;
	genie |= temp;

	if (compare != -1) {

		temp = (compare & 0x8) | ((address & 0x700) >> 8);
		genie <<= 4;
		genie |= temp;

		temp = ((compare & 0x80) >> 4) | (compare & 0x7);
		genie <<= 4;
		genie |= temp;

		temp = (value & 0x8) | ((compare & 0x70) >> 4);
		genie <<= 4;
		genie |= temp;
	} else {
		temp = (value & 0x8) | ((address & 0x700) >> 8);
		genie <<= 4;
		genie |= temp;
	}

	result[6] = result[7] = result[8] = 0;

	for (int i = 0; i < ((compare != -1) ? 8 : 6); i++) {
		result[((compare != -1) ? 8 : 6) - 1 - i] = ALPHABET_NES[address_lower][(genie >> (i * 4)) & 0xF];
	}

	return 0;
}

// VirtuaNES .vct format
static INT32 ConfigParseVCT(TCHAR* pszFilename)
{
#define AddressInfoGameGenie() { \
		pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nTotalByte = 1;	\
		pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].nAddress = 0xffff; \
		strcpy(pCurrentCheat->pOption[n]->AddressInfo[nCurrentAddress].szGenieCode, szGGenie); \
		nCurrentAddress++;	\
	}

#define OptionName(a)	\
	if (pCurrentCheat->pOption[n] == NULL) {						\
		pCurrentCheat->pOption[n] = (CheatOption*)malloc(sizeof(CheatOption));		\
	}											\
	memset(pCurrentCheat->pOption[n], 0, sizeof(CheatOption));				\
	_tcsncpy (pCurrentCheat->pOption[n]->szOptionName, a, QUOTE_MAX * sizeof(TCHAR));	\

#define tmpcpy(a)	\
	_tcsncpy (tmp, szLine + c0[a] + 1, c0[a+1] - (c0[a]+1));	\
	tmp[c0[a+1] - (c0[a]+1)] = '\0';				\

	TCHAR tmp[256];
	TCHAR szLine[1024];
	char szGGenie[256] = { 0, };

	INT32 nLen;
	INT32 n = 0;
	INT32 nCurrentAddress = 0;

	bool bFirst = true;

	TCHAR* text = conc_load_text(pszFilename);
	TCHAR* pszFileHeading = getFilenameFromPath(pszFilename);
	if (text == NULL) {
		if ((BurnDrvGetFlags() & BDF_CLONE) && BurnDrvGetText(DRV_PARENT)) {
			TCHAR szAlternative[MAX_PATH] = { 0 };
			_stprintf(szAlternative, _T("%s%s.vct"), szAppCheatsPath, BurnDrvGetText(DRV_PARENT));

			text = conc_load_text(szAlternative);
			if (text == NULL)
				return 1;
			pszFileHeading = getFilenameFromPath(szAlternative);
		} else {
			return 1;
		}
	}

	TCHAR* save = NULL;
	while (conc_gets(szLine, 1024, text, &save) != NULL)
	{
		nLen = _tcslen (szLine);

		while ((nLen > 0) && (szLine[nLen - 1] == 0x0A || szLine[nLen - 1] == 0x0D)) {
			szLine[nLen - 1] = 0;
			nLen--;
		}

		if (szLine[0] == ';') continue;

		INT32 c0[16], c1 = 0, cprev = 0; // find columns / break
		for (INT32 i = 0; i < nLen; i++)
			if ((szLine[i] == ' ' && c1 < 2) || szLine[i] == '\t' || szLine[i] == '\r' || szLine[i] == '\n')
			{
				if (cprev + 1 != i) c0[c1++] = i;
				cprev = i;
			}

		tmpcpy(0);
		strcpy(szGGenie, TCHARToANSI(tmp, NULL, 0));
		szGGenie[255] = '\0';
		tmpcpy(1);

		if (HW_GGENIE) {
			//szGGenie "0077-01-FF" or "0077-04-80808080"
			char temp2[256] = { 0, };
			UINT32 fAddr = 0;
			UINT32 fCount = 0;
			UINT32 fAttr = 0; // always = 0, oneshot = 1, greater = 2 (mem[addr] > bytes), less = 3 (mem[addr] < bytes)
			UINT32 fBytes = 0;

			strcpy(temp2, szGGenie);

			// split up "0077-01-FF" format: "address-[attribute][bytecount]-bytes_to_program"
			char *tok = strtok(temp2, "-");
			if (!tok) continue;
			sscanf(tok, "%x", &fAddr);

			tok = strtok(NULL, "-");
			if (!tok) continue;
			sscanf(tok, "%x", &fCount);
			fAttr = (fCount & 0x30) >> 4;
			fCount &= 0x07;
			if (fCount < 1 || fCount > 4) fCount = 1;

			tok = strtok(NULL, "-");
			if (!tok) continue;
			sscanf(tok, "%x", &fBytes);

			//bprintf(0, _T(".vct: addr[%x] count[%x] bytes[%x]\n"), fAddr, fCount, fBytes);

#ifndef __LIBRETRO__
			if (bFirst) {
				TCHAR szHeading[256];
				_stprintf(szHeading, _T("[ Cheats \"%s\" ]"), pszFileHeading);
				CheatLinkNewNode(szHeading);
				bFirst = false;
			}
#endif

			// -- add to cheat engine --
			n = 0;
			nCurrentAddress = 0;

			CheatLinkNewNode(tmp);

#ifdef __LIBRETRO__
			_tcsncpy (pCurrentCheat->szCheatFilename, pszFileHeading, QUOTE_MAX);
#endif

			OptionName(_T("Disabled"));
			n++;
			OptionName(_T("Enabled"));

			for (int i = 0; i < fCount; i++) {
				memset(szGGenie, 0, sizeof(szGGenie));
				encodeNES(fAddr + i, fBytes >> (i*8), -1, szGGenie);
				INT32 cLen = strlen(szGGenie);
				szGGenie[cLen] = '0' + fAttr; // append the attribute to the end of the GameGenie code (handled in drv/nes/nes.cpp)
				AddressInfoGameGenie();
			}
		}

		continue;
	}

	free(text);

	// if no cheat was found, don't return success code
	if (pCurrentCheat == NULL) return 1;

	return 0;
}


INT32 ConfigCheatLoad()
{
	TCHAR szFilename[MAX_PATH] = _T("");
	TCHAR szDrvName[MAX_PATH] = _T("");

#if defined(BUILD_NEOGEO) || defined(BUILD_PCE)
	_stprintf(szDrvName, _T("%s"), IsCDGame() ? CDInfo_GamePrefix() : BurnDrvGetText(DRV_NAME));
#else
	_stprintf(szDrvName, _T("%s"), BurnDrvGetText(DRV_NAME));
#endif

	bprintf(0, _T("Cheat engine, game name: %s\n"), szDrvName);

	pCurrentCheat = NULL;
	pPreviousCheat = NULL;

	if (HW_NES) { // only for NES/FC!
		_stprintf(szFilename, _T("%s%s.vct"), szAppCheatsPath, szDrvName);
		ConfigParseVCT(szFilename);
	} // keep loading & adding stuff even if .vct file loads.

	// cheat.dat, cheatnes.dat, cheatsnes.dat, wayder_cheat.dat
	ConfigParseMAMEFile(0 /* cheat.dat, cheatnes.dat, cheatsnes.dat */);
	ConfigParseMAMEFile(1 /* wayder */);

	// ini-style file
	_stprintf(szFilename, _T("%s%s.ini"), szAppCheatsPath, szDrvName);
	ConfigParseFile(szFilename);

	// nebula-format .dat file
	_stprintf(szFilename, _T("%s%s.dat"), szAppCheatsPath, szDrvName);
	ConfigParseNebulaFile(szFilename);

	if (pCheatInfo) {
		INT32 nCurrentCheat = 0;
		while (CheatEnable(nCurrentCheat, -1) == 0) {
			nCurrentCheat++;
		}

		CheatUpdate();
	}

	return 0;
}
