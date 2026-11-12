// AT5 Chart Maker - the window program. A front-end for sat2at5.exe (in the same folder or its "program" subfolder):
// pick or drop a map file, adjust a few settings, press Convert. Three steps on white cards, a "?" beside every
// setting (hover for a hint, click for the full explanation), and a paged How-to guide.
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

static const wchar_t* kAppName = L"AT5 Chart Maker";
static const wchar_t* kVersion = L"1.2.1";

enum {
  ID_IN = 100, ID_IN_BROWSE, ID_OUT, ID_OUT_BROWSE, ID_DESC, ID_PROJECT, ID_SCHEME, ID_DEEP, ID_LAND, ID_LAND_BROWSE,
  ID_DRYING, ID_SAFETY, ID_HILITE, ID_FINEST, ID_MORE, ID_MAJOR, ID_FINE, ID_SYNTH, ID_BANDS, ID_OVERVIEW, ID_NAME,
  ID_PVLINES, ID_CONVERT, ID_PREVIEW, ID_VERIFY, ID_OPEN, ID_LOG, ID_GUIDE, ID_ABOUT,
  ID_PALETTE, ID_RANGE, ID_SHADE, ID_CONTOURS, ID_INTERVAL,
  ID_HELP0 = 300,   // "?" buttons: ID_HELP0 + help topic
  TIMER_KIND = 1, TIMER_TICK = 2,
  WM_APP_LOG = WM_APP + 1, WM_APP_DONE, WM_APP_KIND
};

// ---------------------------------------------------------------- help texts (title, short hint, full text)
enum Topic { H_MAP, H_OUT, H_NAME, H_PROJECT, H_STYLE, H_DEEP, H_LAND, H_DRYING, H_SAFETY, H_HILITE, H_FINEST,
             H_MAJOR, H_FINE, H_SYNTH, H_BANDS, H_OVERVIEW, H_VNAME, H_PVLINES,
             H_PALETTE, H_RANGE, H_SHADE, H_CONTOURS, H_COUNT };
struct Help { const wchar_t* title; const wchar_t* hint; const wchar_t* text; };
static const Help kHelp[H_COUNT] = {
  {L"Map file", L"The map to turn into a chart: a depth chart or a picture.",
   L"Three kinds of map work:\n\n"
   L"\x2022  Depth surveys: a GeoTIFF (.tif) of depths, such as a BathyPicker file or a survey download. It "
   L"becomes a coloured, shaded depth picture, and contour lines too if you like.\n\n"
   L"\x2022  Depth charts: vector .mbtiles files with contour lines and/or depth areas (made with QGIS, GDAL or "
   L"tippecanoe, or downloaded survey data). They become coloured depth charts.\n\n"
   L"\x2022  Pictures: satellite photos or coloured bathymetry as .mbtiles (jpg or png tiles), GeoTIFF (.tif), or "
   L".jpg / .png with a world file (.jgw / .pgw) beside it. They become the plotter's imagery layer.\n\n"
   L"Drag the file anywhere onto this window, or press Browse."},
  {L"Save to", L"The folder the chart files go into.",
   L"The finished chart files are written to this folder. It is filled in for you: next to your map file, ending "
   L"in _AT5.\n\nWhen the chart is done, copy this whole folder onto the SD card. Use a new folder for each chart."},
  {L"Chart name", L"The name the plotter tells your charts apart by.",
   L"A short name stored inside the chart files. The plotter uses it to tell your charts apart. It is filled in "
   L"from the map file's name; change it if you like.\n\nThe name and the date are stored in the files (nothing "
   L"else about you or your PC is), so pick a neutral name before sharing charts."},
  {L"Chart name in the file names", L"Lets several charts share one folder on the card.",
   L"Normally every chart uses the same file names (Large.at5, _3DTexture_16.at5 ...), so each chart needs its own "
   L"folder on the SD card.\n\nTick this to put the chart name into the file names (for example Cairns_Large.at5). "
   L"Then several charts can sit in the same folder. This is Insight Map Creator's \"Project Based Naming\"."},
  {L"Colour style", L"How the depths are coloured.",
   L"\x2022  Navionics style (recommended): tan land, a green drying strip, blue that gets lighter with every "
   L"contour, white deep water.\n\n"
   L"\x2022  Paper chart (S-52): the official electronic-chart day colours.\n\n"
   L"\x2022  Soft blue: gentle blue steps, white deep water.\n\n"
   L"\x2022  Custom: your own colours, set under More settings."},
  {L"White from", L"Water deeper than this is white.",
   L"Water deeper than this many metres is white. Shallower water gets one blue step per contour: strong near the "
   L"shore and paler with depth.\n\nRaise it (for example to 50) for more blue steps offshore. Used by the "
   L"Navionics and Soft blue styles."},
  {L"Coastline", L"Draws the land, so water is coloured up to the shore.",
   L"Automatic draws the land from the built-in world coastline (with extra detail around Australia). Land shows "
   L"tan and the water is coloured right up to the shore. It switches itself off for charts that lie inland.\n\n"
   L"Choose None for lakes, dams and rivers, or Browse to use your own coastline shapefile (.shp)."},
  {L"Drying strip", L"The green strip along the shore that dries at low tide.",
   L"Contour files rarely have a 0 m line, so the drying strip is estimated: the coast is taken to be this many "
   L"metres above chart datum. The strip comes out wider on flat shores and thin on steep ones.\n\n"
   L"It is an estimate, not surveyed tide data. 0 = no strip. Needs a coastline."},
  {L"Safety depth", L"Optional: shallower water shown red with a bold line.",
   L"Optional. Water shallower than this many metres is shown in red, with a bold contour line along the edge, "
   L"like a plotter's safety contour.\n\nLeave it empty for none."},
  {L"Highlight", L"Optional: a depth range in a stand-out colour.",
   L"Optional. A depth range to colour in a stand-out colour, for example 10-20 for a favourite fishing depth.\n\n"
   L"Leave it empty for none."},
  {L"Detail", L"How sharp the picture is when zoomed in.",
   L"How much detail the chart keeps when you zoom right in. For pictures, Auto uses the detail your file has. "
   L"For depth surveys, Auto goes one step finer than the survey, so zooming in on the plotter stays smooth.\n\n"
   L"A finer setting (for example 1 m) makes zoomed-in views smoother still, with much bigger files. It can't add "
   L"survey detail that isn't there."},
  {L"Contours when zoomed out", L"Which lines show when zoomed out.",
   L"When zoomed out, only every Nth metre of contour lines is shown, so the chart doesn't turn into a mess of "
   L"lines. Every 5 m by default. All lines show when you zoom in."},
  {L"Finer contours", L"When the in-between lines (e.g. 1 m) appear.",
   L"When the finer contour lines (for example every 1 m) start to show as you zoom in. \"When zoomed in close\" "
   L"matches Insight Map Creator."},
  {L"Colour between contours", L"Builds coloured depth areas between the lines.",
   L"Most contour files only have lines. Automatic builds the coloured depth areas between the lines when the "
   L"file has none of its own.\n\nAlways builds them anyway; Never uses only the file's own depth areas."},
  {L"Custom colours", L"Your own depth colours.",
   L"A list of \"start depth : colour\" pairs, colours as RRGGBB hex, for example:\n\n"
   L"min:83B295, 0:73B6EF, 2:98C5F2, 5:BAD5E1, 10:D4EAEE\n\n"
   L"\"min\" is the drying strip. Each colour runs from its depth down to the next one."},
  {L"Zoomed-out overview", L"A small extra file shown when zoomed far out.",
   L"Also makes Medium.at5, a small overview of the chart that the plotter shows when you zoom far out. Keep it "
   L"on unless you have a reason not to."},
  {L"Detail file name", L"Name of the main depth chart file.",
   L"The name of the main depth chart file: Large, as Insight Map Creator names it. Leave it unless you know you "
   L"need another name."},
  {L"Contour lines in Preview", L"Draw the lines in the preview picture.",
   L"Draws the contour lines in the Preview picture. With dense 1 m contours the lines can hide the colours, so "
   L"it's off by default. It doesn't change the chart itself."},
  {L"Colours", L"The colours used for the depths.",
   L"\x2022  Spectrum: red in the shallows through yellow and green to deep blue. Shows the most detail.\n"
   L"\x2022  Lagoon: pale turquoise shallows to dark blue.\n"
   L"\x2022  Thermal: pale yellow to deep purple.\n"
   L"\x2022  Chart blues: the light blues of a paper chart.\n"
   L"\x2022  Grey: light (shallow) to dark (deep)."},
  {L"Depth range", L"Which depths the colours are spread over.",
   L"The colours are spread over this range of depths, for example 0-200. Water deeper than the range shows "
   L"in the deepest colour.\n\n"
   L"A small range (say 0-30) shows the reefs and shallows in much more detail. A big range (say 0-2000) "
   L"shows the whole drop-off, with fewer colours left for the shallows.\n\n"
   L"Leave it empty for automatic: from 0 to about the deepest water in the survey."},
  {L"Hillshade", L"Shading that makes the seabed look 3D.",
   L"Light from the north-west shades the slopes, so reefs, ridges and channels stand out.\n\n"
   L"Off gives flat colours; Strong makes every bump show. Medium suits most surveys."},
  {L"Contour lines", L"Also makes a chart of depth contour lines.",
   L"Draws depth contour lines from the survey, with their depths written along them, as a separate depth "
   L"chart in the same folder. On the plotter you can show the coloured picture, the lines, or both: the "
   L"picture is the Imagery / Shaded relief overlay, the lines are the chart.\n\n"
   L"Type the spacing in metres (for example 5), a list of depths (2,5,10,20,50), or leave it empty for "
   L"automatic spacing."},
};

// ---------------------------------------------------------------- state
static HWND gWnd, gTip;
static HWND gIn, gInBrowse, gKind, gOut, gOutBrowse, gDesc, gProject;
static HWND gScheme, gDeep, gLand, gLandBrowse, gDrying, gSafety, gHilite, gFinest, gPicNote, gMore;
static HWND gMajor, gFine, gSynth, gBands, gOverview, gName, gPvLines;
static HWND gPalette, gRange, gShade, gContours, gInterval, gBathyNote, lPalette, lRange, lRangeM, lShade, lIntervalM;
static HWND gConvert, gPreview, gVerify, gOpen, gProg, gStatus, gLog, gGuide, gAbout;
static HWND gHelpBtn[H_COUNT];
static HWND lMap, lOut, lName, lStyle, lDeep, lDeepM, lLand, lDry, lDryM, lSafe, lSafeM, lHi, lFinest;
static HWND lMajor, lMajorM, lFine, lSynth, lBands, lVName;
static HFONT gFont, gFontBold, gFontTitle, gFontSection, gFontSmall, gMono;
static HBRUSH gBgBrush, gCardBrush;
static HICON gIcon, gIconSmall;
static HANDLE gJob;
static UINT gDpi = 96;
static bool gBusy, gStopped, gAdvanced;
static int gTask;            // 0 convert, 1 preview, 2 check
static int gKindCode = -1;   // -1 none, 0 depth, 1 picture, 2 problem, 3 depth survey (GeoTIFF of depths)
static ULONGLONG gStart;
static std::wstring gPreviewPng, gLastLine, gKindText;
static COLORREF gKindColor, gStatusColor;
struct Card { RECT rc; int num; const wchar_t* title; std::wstring note; };
static std::vector<Card> gCards;
static int gMinH = 600;

static const COLORREF kBg = RGB(243, 245, 248), kCard = RGB(255, 255, 255), kBorder = RGB(221, 226, 232);
static const COLORREF kAccent = RGB(0, 99, 177), kAccentDark = RGB(0, 78, 140), kText = RGB(28, 30, 33);
static const COLORREF kMuted = RGB(98, 106, 116), kOk = RGB(16, 124, 65), kWarn = RGB(190, 50, 30), kStop = RGB(196, 43, 28);

static int S(int v) { return MulDiv(v, int(gDpi), 96); }

static std::wstring GetText(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring s(size_t(n) + 1, L'\0');
  GetWindowTextW(h, &s[0], n + 1);
  s.resize(size_t(n));
  return s;
}
static std::wstring ExeDir() {
  wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring s(p); return s.substr(0, s.find_last_of(L"\\/"));
}
static std::wstring Lower(std::wstring s) { for (auto& c : s) c = wchar_t(towlower(c)); return s; }
static std::wstring Stem(const std::wstring& path);
static std::wstring Trim(std::wstring s) {
  while (!s.empty() && iswspace(s.back())) s.pop_back();
  size_t a = 0; while (a < s.size() && iswspace(s[a])) ++a;
  return s.substr(a);
}

// the converter: next to this program, or in its "program" subfolder (the shared package layout)
static std::wstring EngineExe() {
  for (const wchar_t* sub : {L"\\program\\sat2at5.exe", L"\\sat2at5.exe"}) {
    const std::wstring p = ExeDir() + sub;
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  return ExeDir() + L"\\program\\sat2at5.exe";
}

// quoted command-line argument; trailing backslashes are doubled so "E:\" does not end in an escaped quote
static std::wstring Q(const std::wstring& s) {
  size_t n = 0;
  while (n < s.size() && s[s.size() - 1 - n] == L'\\') ++n;
  return L"\"" + s + std::wstring(n, L'\\') + L"\"";
}

// coastline choices; a coastline file the user picks becomes a third entry
static const wchar_t* kLandAuto = L"Automatic - built-in world coastline";
static const wchar_t* kLandNone = L"None - for lakes, dams and rivers";
// Depth band colours: "start depth (m) : RRGGBB". S-52 = IHO ECDIS day colours (DEPIT green, DEPVS/DEPMS/DEPMD blues, DEPDW).
static const wchar_t* kBandsS52 = L"min:83B295,0:73B6EF,2:98C5F2,5:BAD5E1,10:D4EAEE";

// ---------------------------------------------------------------- dialogs
static void InfoBox(const wchar_t* title, const std::wstring& text, PCWSTR icon = TD_INFORMATION_ICON) {
  TASKDIALOGCONFIG c{sizeof c};
  c.hwndParent = gWnd; c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
  c.pszWindowTitle = kAppName; c.pszMainInstruction = title; c.pszContent = text.c_str();
  c.pszMainIcon = icon; c.dwCommonButtons = TDCBF_OK_BUTTON;
  TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
}
static bool AskBox(const wchar_t* title, const std::wstring& text, const wchar_t* yes, const wchar_t* no) {
  TASKDIALOG_BUTTON b[] = {{IDYES, yes}, {IDNO, no}};
  TASKDIALOGCONFIG c{sizeof c};
  c.hwndParent = gWnd; c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
  c.pszWindowTitle = kAppName; c.pszMainInstruction = title; c.pszContent = text.c_str();
  c.pszMainIcon = TD_WARNING_ICON; c.cButtons = 2; c.pButtons = b; c.nDefaultButton = IDNO;
  int r = IDNO;
  TaskDialogIndirect(&c, &r, nullptr, nullptr);
  return r == IDYES;
}

struct GuidePage { const wchar_t* title; const wchar_t* text; };
static const GuidePage kGuide[] = {
  {L"Welcome to AT5 Chart Maker",
   L"Make your own charts for Simrad, Lowrance and B&G chartplotters, in the same .at5 format as Navico's "
   L"Insight Map Creator.\n\n"
   L"\x2022  Depth charts from contour lines (1 m, 5 m ...), coloured like Navionics: tan land, a green drying "
   L"strip, blue getting lighter with depth, white deep water.\n"
   L"\x2022  Satellite photos or coloured bathymetry as the plotter's imagery layer.\n"
   L"\x2022  Depth surveys (a GeoTIFF of depths, e.g. from BathyPicker): a coloured, shaded depth picture, with "
   L"contour lines on top if you like.\n\n"
   L"You need: your map file, and an SD / microSD card of 32 GB or smaller, formatted FAT32.\n\n"
   L"Not for navigation. These are home-made charts; always carry official charts."},
  {L"Step 1 \x2013 Choose your map",
   L"Drag your map file anywhere onto the window, or press Browse next to Map file.\n\n"
   L"The program works out what kind of map it is and shows only the settings that apply. The Save to folder "
   L"and the Chart name fill in by themselves.\n\n"
   L"Depth surveys: a GeoTIFF (.tif) of depths, for example from BathyPicker.\n"
   L"Depth charts: vector .mbtiles with contour lines or depth areas.\n"
   L"Pictures: .mbtiles with jpg/png tiles, GeoTIFF (.tif), or .jpg/.png with a world file."},
  {L"Step 2 \x2013 Pick the look",
   L"For depth charts, the defaults give the Navionics look and suit almost every file. Things you may want to "
   L"change:\n\n"
   L"\x2022  White from: deeper water is white. Raise it for more blue steps offshore.\n"
   L"\x2022  Coastline: keep Automatic for the sea; choose None for lakes, dams and rivers.\n"
   L"\x2022  Safety depth / Highlight: optional extras.\n\n"
   L"Not sure what something does? Hover over the ? beside it, or click it for the full story."},
  {L"Step 3 \x2013 Make the chart",
   L"Press Convert. A small area takes seconds; a whole country can take 10\x2013" L"15 minutes. You can keep "
   L"using your PC, and the Stop button cancels.\n\n"
   L"When it says Done:\n"
   L"\x2022  Preview shows a picture of the finished chart.\n"
   L"\x2022  Check files tests the chart files (it should find 0 problems).\n"
   L"\x2022  Open folder shows the files."},
  {L"Step 4 \x2013 Put it on the SD card",
   L"Copy the whole output folder (for example \"Cairns_AT5\") onto the SD card. Rename the folder if you like, "
   L"but don't rename the files inside it.\n\n"
   L"Put the card in the plotter, and in the chart settings choose Lowrance / Insight as the chart source (not "
   L"Navionics or C-MAP). For satellite or bathymetry pictures also turn on the Imagery / Shaded relief overlay.\n\n"
   L"Several charts: give each chart its own folder on the card."},
  {L"Tips and fixes",
   L"\x2022  Nothing shows on the plotter: check the chart source is Lowrance / Insight and the card is FAT32. "
   L"Some older units only read files at the top of the card: copy the .at5 files themselves to the top.\n\n"
   L"\x2022  Windows says \"Windows protected your PC\": click More info, then Run anyway (the program isn't "
   L"signed by a big company).\n\n"
   L"\x2022  Convert reports a problem: the messages box at the bottom says why.\n\n"
   L"\x2022  Try a small area first, before converting a whole country."},
};
static const int kGuidePages = int(sizeof kGuide / sizeof kGuide[0]);
static TASKDIALOGCONFIG gGuideCfg[sizeof kGuide / sizeof kGuide[0]];
static std::wstring gGuideFooter[sizeof kGuide / sizeof kGuide[0]];
static TASKDIALOG_BUTTON gGuideBtn[sizeof kGuide / sizeof kGuide[0]][2];
static int gGuidePage;
enum { ID_GUIDE_BACK = 1001, ID_GUIDE_NEXT };

static HRESULT CALLBACK GuideProc(HWND h, UINT n, WPARAM w, LPARAM, LONG_PTR) {
  if (n == TDN_BUTTON_CLICKED && (w == ID_GUIDE_NEXT || w == ID_GUIDE_BACK)) {
    gGuidePage = std::max(0, std::min(kGuidePages - 1, gGuidePage + (w == ID_GUIDE_NEXT ? 1 : -1)));
    SendMessageW(h, TDM_NAVIGATE_PAGE, 0, (LPARAM)&gGuideCfg[gGuidePage]);
    return S_FALSE;
  }
  return S_OK;
}
static void ShowGuide() {
  for (int i = 0; i < kGuidePages; ++i) {
    TASKDIALOGCONFIG& c = gGuideCfg[i];
    c = TASKDIALOGCONFIG{sizeof c};
    c.hwndParent = gWnd;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_USE_HICON_MAIN | TDF_SIZE_TO_CONTENT;
    c.pszWindowTitle = L"How to use AT5 Chart Maker";
    c.hMainIcon = gIcon;
    c.pszMainInstruction = kGuide[i].title;
    c.pszContent = kGuide[i].text;
    gGuideFooter[i] = L"Page " + std::to_wstring(i + 1) + L" of " + std::to_wstring(kGuidePages);
    c.pszFooter = gGuideFooter[i].c_str();
    int nb = 0;
    if (i > 0) gGuideBtn[i][nb++] = {ID_GUIDE_BACK, L"Back"};
    if (i + 1 < kGuidePages) gGuideBtn[i][nb++] = {ID_GUIDE_NEXT, L"Next"};
    c.cButtons = UINT(nb); c.pButtons = gGuideBtn[i];
    c.nDefaultButton = i + 1 < kGuidePages ? ID_GUIDE_NEXT : IDCLOSE;
    c.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    c.pfCallback = GuideProc;
    c.cxWidth = 270;
  }
  gGuidePage = 0;
  TaskDialogIndirect(&gGuideCfg[0], nullptr, nullptr, nullptr);
}
static void ShowAbout() {
  TASKDIALOGCONFIG c{sizeof c};
  c.hwndParent = gWnd;
  c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_USE_HICON_MAIN;
  c.pszWindowTitle = L"About";
  c.hMainIcon = gIcon;
  const std::wstring title = std::wstring(kAppName) + L" " + kVersion;
  c.pszMainInstruction = title.c_str();
  c.pszContent =
      L"Makes Simrad / Lowrance / B&G chart files (.at5) from depth contours and from satellite or bathymetry "
      L"pictures. Free software; works offline and sends nothing anywhere.\n\n"
      L"Coastlines: GSHHG 2.3.7 (Wessel & Smith, University of Hawaii / NOAA, LGPL 3) and GEODATA COAST 100K 2004 "
      L"(\x00A9" L" Commonwealth of Australia, Geoscience Australia, CC BY 4.0). Details in the \"Licences and "
      L"credits\" folder.\n\n"
      L"Not affiliated with Navico, Simrad, Lowrance, B&G or Navionics. Not for navigation: always carry official "
      L"charts.";
  c.dwCommonButtons = TDCBF_OK_BUTTON;
  TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------- controls
static void AddTip(HWND target, const wchar_t* text) {
  TOOLINFOW ti{sizeof ti};
  ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS; ti.hwnd = gWnd; ti.uId = (UINT_PTR)target; ti.lpszText = const_cast<wchar_t*>(text);
  SendMessageW(gTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}
static HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
  HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, gWnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
  SendMessageW(c, WM_SETFONT, (WPARAM)gFont, FALSE);
  return c;
}
static HWND Label(const wchar_t* t) { return Make(L"STATIC", t, SS_LEFT | SS_NOPREFIX, 0); }
static HWND EditBox(int id, const wchar_t* cue = nullptr) {
  HWND c = Make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, id, WS_EX_CLIENTEDGE);
  if (cue) SendMessageW(c, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
  return c;
}
static HWND Combo(int id, std::initializer_list<const wchar_t*> items, int sel) {
  HWND c = Make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, id);
  for (auto s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s);
  SendMessageW(c, CB_SETCURSEL, sel, 0);
  return c;
}
static HWND Btn(const wchar_t* t, int id, DWORD style = BS_PUSHBUTTON) { return Make(L"BUTTON", t, WS_TABSTOP | style, id); }
static HWND Check(const wchar_t* t, int id, bool on) {
  HWND c = Btn(t, id, BS_AUTOCHECKBOX);
  SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
  return c;
}
static HWND HelpBtn(Topic t) {
  HWND c = Make(L"BUTTON", L"?", BS_OWNERDRAW, ID_HELP0 + t);
  gHelpBtn[t] = c;
  AddTip(c, kHelp[t].hint);
  return c;
}
static bool IsLink(HWND h) { return h == gGuide || h == gAbout || h == gMore; }
static bool IsHelp(HWND h) { return GetDlgCtrlID(h) >= ID_HELP0 && GetDlgCtrlID(h) < ID_HELP0 + H_COUNT; }

static void MakeFonts() {
  for (HFONT* f : {&gFont, &gFontBold, &gFontTitle, &gFontSection, &gFontSmall, &gMono}) if (*f) { DeleteObject(*f); *f = nullptr; }
  auto font = [](int pt10, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(pt10, int(gDpi), 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
  };
  gFont = font(95, FW_NORMAL, L"Segoe UI");
  gFontBold = font(95, FW_SEMIBOLD, L"Segoe UI");
  gFontTitle = font(170, FW_SEMIBOLD, L"Segoe UI");
  gFontSection = font(115, FW_SEMIBOLD, L"Segoe UI");
  gFontSmall = font(90, FW_NORMAL, L"Segoe UI");
  gMono = font(90, FW_NORMAL, L"Consolas");
}
static BOOL CALLBACK SetFontProc(HWND c, LPARAM) {
  HFONT f = gFont;
  if (c == gLog) f = gMono;
  else if (c == gStatus) f = gFontBold;
  else if (c == gPicNote || c == gBathyNote) f = gFontSmall;
  SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
  return TRUE;
}

// ---------------------------------------------------------------- layout
static void Put(HWND h, int x, int y, int w, int hh, bool show = true) {
  SetWindowPos(h, nullptr, x, y, std::max(w, 1), std::max(hh, 1), SWP_NOZORDER | SWP_NOACTIVATE);
  ShowWindow(h, show ? SW_SHOWNA : SW_HIDE);
}
static void Hide(std::initializer_list<HWND> hs) { for (HWND h : hs) ShowWindow(h, SW_HIDE); }

static const int kLeftW = 660;   // left column (steps 1 and 2), DIP

// Places every control for client width W / height H (two columns: steps 1-2 left, step 3 right);
// returns the client height the content needs.
static int Layout(int W, int H) {
  const int M = S(16), P = S(18), G = S(8), RH = S(26), LW = S(104), HB = S(20), BW = S(92), GAP = S(12), ROW = RH + S(10);
  const bool bathy = gKindCode == 3;
  const bool depth = gKindCode != 1 && !bathy;
  gCards.clear();
  Put(gAbout, W - M - S(56), S(20), S(56), S(24));
  Put(gGuide, W - M - S(56) - S(110), S(20), S(110), S(24));
  const int headH = S(70);
  const int L0 = M, L1 = M + S(kLeftW);              // left column card edges
  const int R0 = L1 + GAP, R1 = std::max(W - M, R0 + S(300));
  const int x0 = L0 + P, right = L1 - P;
  auto help = [&](Topic t, int x, int yy) { Put(gHelpBtn[t], x, yy + (RH - HB) / 2, HB, HB); };
  auto label = [&](HWND l, int x, int yy, int w) { Put(l, x, yy + S(4), w, S(20)); };
  auto unit = [&](HWND l, int& cx, int yy) { label(l, cx, yy, S(18)); cx += S(18); };

  // 1  Your map
  int y = headH, top = y;
  y += S(46);
  label(lMap, x0, y, LW);
  help(H_MAP, right - HB, y);
  Put(gInBrowse, right - HB - G - BW, y - S(1), BW, RH + S(2));
  Put(gIn, x0 + LW, y, right - HB - G - BW - G - (x0 + LW), RH);
  y += RH + S(4);
  Put(gKind, x0 + LW, y, right - (x0 + LW), S(20));
  y += S(20) + S(8);
  label(lOut, x0, y, LW);
  help(H_OUT, right - HB, y);
  Put(gOutBrowse, right - HB - G - BW, y - S(1), BW, RH + S(2));
  Put(gOut, x0 + LW, y, right - HB - G - BW - G - (x0 + LW), RH);
  y += ROW;
  label(lName, x0, y, LW);
  Put(gDesc, x0 + LW, y, S(190), RH);
  help(H_NAME, x0 + LW + S(190) + G, y);
  {
    const int cx = x0 + LW + S(190) + G + HB + S(24);
    Put(gProject, cx, y + S(2), right - HB - G - cx, S(22));
    help(H_PROJECT, right - HB, y);
  }
  y += RH + P - S(2);
  gCards.push_back({{L0, top, L1, y}, 1, L"Your map", L""});
  y += GAP;

  // 2  Chart look
  top = y;
  y += S(46);
  const std::initializer_list<HWND> bathyCtl = {lPalette, gPalette, gHelpBtn[H_PALETTE], lRange, gRange, lRangeM, gHelpBtn[H_RANGE],
                                                lShade, gShade, gHelpBtn[H_SHADE], gContours, gInterval, lIntervalM,
                                                gHelpBtn[H_CONTOURS], gBathyNote};
  if (bathy) {
    Hide({lStyle, gScheme, gHelpBtn[H_STYLE], lDeep, gDeep, lDeepM, gHelpBtn[H_DEEP], lDry, gDrying, lDryM, gHelpBtn[H_DRYING],
          lSafe, gSafety, lSafeM, gHelpBtn[H_SAFETY], lHi, gHilite, gHelpBtn[H_HILITE], gMore, lMajor, gMajor, lMajorM,
          gHelpBtn[H_MAJOR], lFine, gFine, gHelpBtn[H_FINE], lSynth, gSynth, gHelpBtn[H_SYNTH], lBands, gBands,
          gHelpBtn[H_BANDS], gOverview, gHelpBtn[H_OVERVIEW], lVName, gName, gHelpBtn[H_VNAME], gPvLines,
          gHelpBtn[H_PVLINES], gPicNote});
    label(lPalette, x0, y, LW);
    Put(gPalette, x0 + LW, y, S(230), S(300));
    help(H_PALETTE, x0 + LW + S(230) + G, y);
    {
      int cx = x0 + LW + S(230) + G + HB + S(28);
      label(lRange, cx, y, S(84)); cx += S(84);
      Put(gRange, cx, y, S(80), RH); cx += S(80) + S(4);
      unit(lRangeM, cx, y);
      help(H_RANGE, cx, y);
    }
    y += ROW;
    label(lShade, x0, y, LW);
    Put(gShade, x0 + LW, y, S(230), S(300));
    help(H_SHADE, x0 + LW + S(230) + G, y);
    {
      int cx = x0 + LW + S(230) + G + HB + S(28);
      label(lFinest, cx, y, S(84)); cx += S(84);
      Put(gFinest, cx, y, std::max(S(120), right - HB - G - cx), S(300));
      help(H_FINEST, right - HB, y);
    }
    y += ROW;
    {
      int cx = x0;
      Put(gContours, cx, y + S(2), S(212), S(22)); cx += S(212);
      Put(gInterval, cx, y, S(110), RH); cx += S(110) + S(4);
      unit(lIntervalM, cx, y);
      help(H_CONTOURS, cx, y);
    }
    y += ROW;
    label(lLand, x0, y, LW);
    help(H_LAND, right - HB, y);
    Put(gLandBrowse, right - HB - G - BW, y - S(1), BW, RH + S(2));
    Put(gLand, x0 + LW, y, right - HB - G - BW - G - (x0 + LW), S(300));
    EnableWindow(gLand, SendMessageW(gContours, BM_GETCHECK, 0, 0) == BST_CHECKED);
    EnableWindow(gLandBrowse, SendMessageW(gContours, BM_GETCHECK, 0, 0) == BST_CHECKED);
    y += ROW;
    Put(gBathyNote, x0, y, right - x0, S(40));
    y += S(40) - S(8);
  } else if (depth) {
    Hide(bathyCtl);
    EnableWindow(gLand, TRUE); EnableWindow(gLandBrowse, TRUE);
    label(lStyle, x0, y, LW);
    Put(gScheme, x0 + LW, y, S(230), S(300));
    help(H_STYLE, x0 + LW + S(230) + G, y);
    {
      int cx = x0 + LW + S(230) + G + HB + S(28);
      label(lDeep, cx, y, S(74)); cx += S(74);
      Put(gDeep, cx, y, S(48), RH); cx += S(48) + S(4);
      unit(lDeepM, cx, y);
      help(H_DEEP, cx, y);
    }
    y += ROW;
    label(lLand, x0, y, LW);
    help(H_LAND, right - HB, y);
    Put(gLandBrowse, right - HB - G - BW, y - S(1), BW, RH + S(2));
    Put(gLand, x0 + LW, y, right - HB - G - BW - G - (x0 + LW), S(300));
    y += ROW;
    {
      int cx = x0;
      label(lDry, cx, y, LW); cx += LW;
      Put(gDrying, cx, y, S(48), RH); cx += S(48) + S(4);
      unit(lDryM, cx, y);
      help(H_DRYING, cx, y); cx += HB + S(26);
      label(lSafe, cx, y, S(86)); cx += S(86);
      Put(gSafety, cx, y, S(52), RH); cx += S(52) + S(4);
      unit(lSafeM, cx, y);
      help(H_SAFETY, cx, y); cx += HB + S(26);
      label(lHi, cx, y, S(64)); cx += S(64);
      Put(gHilite, cx, y, std::max(S(60), right - HB - G - cx), RH);
      help(H_HILITE, right - HB, y);
    }
    y += RH + S(8);
    Put(gMore, x0 - S(2), y, S(170), S(24));
    y += S(24);
    Hide({lFinest, gFinest, gHelpBtn[H_FINEST], gPicNote});
    if (gAdvanced) {
      y += S(10);
      const int LW2 = S(180);
      label(lMajor, x0, y, LW2);
      {
        int cx = x0 + LW2;
        Put(gMajor, cx, y, S(48), RH); cx += S(48) + S(4);
        unit(lMajorM, cx, y);
        help(H_MAJOR, cx, y); cx += HB + S(26);
        label(lFine, cx, y, S(140)); cx += S(140);
        Put(gFine, cx, y, std::max(S(100), right - HB - G - cx), S(300));
        help(H_FINE, right - HB, y);
      }
      y += ROW;
      label(lSynth, x0, y, LW2);
      Put(gSynth, x0 + LW2, y, S(300), S(300));
      help(H_SYNTH, x0 + LW2 + S(300) + G, y);
      y += ROW;
      label(lBands, x0, y, LW2);
      help(H_BANDS, right - HB, y);
      Put(gBands, x0 + LW2, y, right - HB - G - (x0 + LW2), RH);
      y += ROW;
      Put(gOverview, x0, y + S(2), S(262), S(22));
      help(H_OVERVIEW, x0 + S(262) + G, y);
      {
        int cx = x0 + S(262) + G + HB + S(26);
        label(lVName, cx, y, S(110)); cx += S(110);
        Put(gName, cx, y, std::max(S(60), right - HB - G - cx), RH);
        help(H_VNAME, right - HB, y);
      }
      y += ROW;
      Put(gPvLines, x0, y + S(2), S(262), S(22));
      help(H_PVLINES, x0 + S(262) + G, y);
      y += RH;
    } else {
      Hide({lMajor, gMajor, lMajorM, gHelpBtn[H_MAJOR], lFine, gFine, gHelpBtn[H_FINE], lSynth, gSynth, gHelpBtn[H_SYNTH],
            lBands, gBands, gHelpBtn[H_BANDS], gOverview, gHelpBtn[H_OVERVIEW], lVName, gName, gHelpBtn[H_VNAME],
            gPvLines, gHelpBtn[H_PVLINES]});
    }
  } else {
    Hide(bathyCtl);
    Hide({lStyle, gScheme, gHelpBtn[H_STYLE], lDeep, gDeep, lDeepM, gHelpBtn[H_DEEP], lLand, gLand, gLandBrowse,
          gHelpBtn[H_LAND], lDry, gDrying, lDryM, gHelpBtn[H_DRYING], lSafe, gSafety, lSafeM, gHelpBtn[H_SAFETY], lHi,
          gHilite, gHelpBtn[H_HILITE], gMore, lMajor, gMajor, lMajorM, gHelpBtn[H_MAJOR], lFine, gFine, gHelpBtn[H_FINE],
          lSynth, gSynth, gHelpBtn[H_SYNTH], lBands, gBands, gHelpBtn[H_BANDS], gOverview, gHelpBtn[H_OVERVIEW], lVName,
          gName, gHelpBtn[H_VNAME], gPvLines, gHelpBtn[H_PVLINES]});
    label(lFinest, x0, y, LW);
    Put(gFinest, x0 + LW, y, S(220), S(300));
    help(H_FINEST, x0 + LW + S(220) + G, y);
    y += ROW;
    Put(gPicNote, x0, y, right - x0, S(40));
    y += S(40) - S(8);
  }
  y += P - S(2);
  gCards.push_back({{L0, top, L1, y}, 2, L"Chart look",
                    gKindCode == 1 ? L"Pictures keep their own colours" : bathy ? L"Colours, shading and contour lines"
                                                                                 : L"The defaults suit almost every file"});
  const int leftBottom = y;

  // 3  Make the chart (right column)
  const int rx0 = R0 + P, rright = R1 - P, rw = rright - rx0;
  y = headH + S(46);
  Put(gConvert, rx0, y, rw, S(40));
  y += S(40) + S(10);
  {
    const int bw = (rw - 2 * G) / 3;
    int cx = rx0;
    for (HWND b : {gPreview, gVerify, gOpen}) { Put(b, cx, y, bw, S(30)); cx += bw + G; }
  }
  y += S(30) + S(16);
  Put(gProg, rx0, y, rw, S(6), IsWindowVisible(gProg) != 0);
  y += S(6) + S(10);
  Put(gStatus, rx0, y, rw, S(42));
  y += S(42) + S(8);
  const int logTop = y;
  const int needed = std::max(leftBottom, logTop + S(140) + P) + M;
  const int bottom = std::max(H - M, logTop + S(60) + P);
  Put(gLog, rx0, logTop, rw, bottom - P - logTop);
  gCards.push_back({{R0, headH, R1, bottom}, 3, L"Make the chart", L""});
  InvalidateRect(gWnd, nullptr, TRUE);
  return needed;
}
static void Relayout(bool fit = false) {
  RECT rc; GetClientRect(gWnd, &rc);
  gMinH = Layout(rc.right, rc.bottom);
  if (fit && rc.bottom != gMinH && !IsZoomed(gWnd)) {   // window height follows the content
    RECT wr; GetWindowRect(gWnd, &wr);
    HMONITOR mon = MonitorFromWindow(gWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi);
    const int hgt = std::min<int>(wr.bottom - wr.top + (gMinH - rc.bottom), mi.rcWork.bottom - mi.rcWork.top);
    const int top = std::max<int>(mi.rcWork.top, std::min<int>(wr.top, mi.rcWork.bottom - hgt));
    SetWindowPos(gWnd, nullptr, wr.left, top, wr.right - wr.left, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

// ---------------------------------------------------------------- painting
static void RoundBox(HDC dc, const RECT& r, int rad, COLORREF fill, COLORREF line) {
  HBRUSH b = CreateSolidBrush(fill);
  HPEN p = CreatePen(PS_SOLID, 1, line);
  HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
  RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
  SelectObject(dc, ob); SelectObject(dc, op);
  DeleteObject(b); DeleteObject(p);
}
static void Paint(HDC dc, const RECT& rc) {
  FillRect(dc, &rc, gBgBrush);
  SetBkMode(dc, TRANSPARENT);
  const int M = S(16);
  DrawIconEx(dc, M, S(16), gIcon, S(40), S(40), 0, nullptr, DI_NORMAL);
  SelectObject(dc, gFontTitle); SetTextColor(dc, kText);
  RECT t{M + S(52), S(13), rc.right - S(200), S(42)};
  DrawTextW(dc, kAppName, -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(dc, gFont); SetTextColor(dc, kMuted);
  RECT st{M + S(52), S(42), rc.right - S(200), S(62)};
  DrawTextW(dc, L"Your own charts for Simrad, Lowrance and B&G chartplotters", -1, &st, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
  for (auto& c : gCards) {
    RoundBox(dc, c.rc, S(14), kCard, kBorder);
    const int bx = c.rc.left + S(18), by = c.rc.top + S(16), bd = S(24);
    HBRUSH ab = CreateSolidBrush(kAccent);
    HGDIOBJ ob = SelectObject(dc, ab), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, bx, by, bx + bd, by + bd);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(ab);
    SelectObject(dc, gFontBold); SetTextColor(dc, RGB(255, 255, 255));
    RECT nr{bx, by, bx + bd, by + bd};
    const wchar_t num[2] = {wchar_t(L'0' + c.num), 0};
    DrawTextW(dc, num, -1, &nr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, gFontSection); SetTextColor(dc, kText);
    RECT tr{bx + bd + S(10), by - S(2), c.rc.right - S(18), by + bd + S(2)};
    DrawTextW(dc, c.title, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (!c.note.empty()) {
      SelectObject(dc, gFontSmall); SetTextColor(dc, kMuted);
      DrawTextW(dc, c.note.c_str(), -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
  }
}
static void DrawOwnerButton(const DRAWITEMSTRUCT* d) {
  HDC dc = d->hDC;
  RECT r = d->rcItem;
  const bool pressed = d->itemState & ODS_SELECTED, disabled = d->itemState & ODS_DISABLED;
  const bool onCard = !(d->hwndItem == gGuide || d->hwndItem == gAbout);
  FillRect(dc, &r, onCard ? gCardBrush : gBgBrush);
  SetBkMode(dc, TRANSPARENT);
  if (d->hwndItem == gConvert) {
    const bool stop = gBusy && gTask == 0;
    COLORREF f = disabled ? RGB(160, 170, 180) : stop ? kStop : pressed ? kAccentDark : kAccent;
    RoundBox(dc, r, S(10), f, f);
    SelectObject(dc, gFontBold); SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, stop ? L"Stop" : L"Convert", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  } else if (IsHelp(d->hwndItem)) {
    const int s = std::min(r.right - r.left, r.bottom - r.top);
    RECT c{r.left, r.top, r.left + s, r.top + s};
    HBRUSH b = CreateSolidBrush(pressed ? RGB(220, 232, 244) : RGB(238, 242, 247));
    HPEN p = CreatePen(PS_SOLID, 1, RGB(176, 190, 206));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Ellipse(dc, c.left, c.top, c.right, c.bottom);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
    SelectObject(dc, gFontBold); SetTextColor(dc, kAccent);
    DrawTextW(dc, L"?", -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  } else {   // link-style buttons
    SelectObject(dc, gFontBold); SetTextColor(dc, pressed ? kAccentDark : kAccent);
    const std::wstring t = GetText(d->hwndItem);
    DrawTextW(dc, t.c_str(), -1, &r, (d->hwndItem == gMore ? DT_LEFT : DT_RIGHT) | DT_VCENTER | DT_SINGLELINE);
  }
  if (d->itemState & ODS_FOCUS && !(d->itemState & ODS_NOFOCUSRECT)) {   // keyboard focus: a thin ring in the text colour
    RECT f = r; InflateRect(&f, -S(3), -S(3));
    HPEN pen = CreatePen(PS_SOLID, 1, d->hwndItem == gConvert ? RGB(255, 255, 255) : kAccent);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, f.left, f.top, f.right, f.bottom, S(8), S(8));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
  }
}

// ---------------------------------------------------------------- kind of input (asks the converter)
static std::string RunCapture(const std::wstring& args) {
  std::string out;
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE rd, wr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return out;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = Q(EngineExe()) + L" " + args;
  if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(wr); wr = nullptr;
    char buf[512]; DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  }
  if (wr) CloseHandle(wr);
  CloseHandle(rd);
  return out;
}
static void DetectKind() {
  const std::wstring in = Trim(GetText(gIn));
  if (in.empty()) {
    gKindCode = -1;
    gKindText = L"Drop a map file anywhere on this window, or press Browse.";
    gKindColor = kMuted;
    SetWindowTextW(gKind, gKindText.c_str());
    if (IsWindowVisible(gWnd)) Relayout(true);
    return;
  }
  std::thread([in] {
    const std::string r = RunCapture(L"info " + Q(in));
    int code = 9;
    if (r.rfind("bathymetry", 0) == 0) code = 7;
    else if (r.rfind("depth", 0) == 0) code = 0;
    else if (r.rfind("picture", 0) == 0) code = 1;
    else if (r.rfind("elevation", 0) == 0) code = 2;
    else if (r.rfind("invalid", 0) == 0) code = 3;
    else if (r.rfind("missing", 0) == 0) code = 4;
    else if (r.rfind("folder", 0) == 0) code = 5;
    else if (r.rfind("unknown", 0) == 0) code = 6;
    PostMessageW(gWnd, WM_APP_KIND, WPARAM(code), 0);
  }).detach();
}
static void OnKind(int code) {
  static const wchar_t* text[] = {
      L"\x2713  Depth chart \x2013 the contours become a coloured depth chart",
      L"\x2713  Picture map \x2013 becomes the plotter's imagery / shaded-relief layer",
      L"This file holds heights stored as colours (terrain-RGB). Make contour lines from it first.",
      L"This isn't an MBTiles file.",
      L"File not found.",
      L"That's a folder \x2013 choose a map file inside it.",
      L"This file type isn't supported. Use .mbtiles, .tif, or .jpg/.png with a world file.",
      L"\x2713  Depth survey \x2013 becomes a coloured, shaded depth picture, with contour lines if you like",
  };
  gKindCode = code == 0 ? 0 : code == 1 ? 1 : code == 7 ? 3 : 2;
  const bool good = code <= 1 || code == 7;
  if (good && Trim(GetText(gOut)).empty()) {
    const std::wstring in = Trim(GetText(gIn));
    const size_t slash = in.find_last_of(L"\\/");
    SetWindowTextW(gOut, ((slash == std::wstring::npos ? L"." : in.substr(0, slash)) + L"\\" + Stem(in) + L"_AT5").c_str());
  }
  gKindText = code >= 0 && code <= 7 ? text[code] : L"Could not read this file.";
  gKindColor = good ? kOk : kWarn;
  SetWindowTextW(gKind, gKindText.c_str());
  Relayout(true);
}

// ---------------------------------------------------------------- input / output names
static std::wstring gAutoDesc;   // chart name last filled in from an input file name
static std::wstring Stem(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/"), dot = path.find_last_of(L'.');
  if (slash == std::wstring::npos) slash = size_t(-1);
  return path.substr(slash + 1, (dot == std::wstring::npos || (slash != size_t(-1) && dot < slash) ? path.size() : dot) - (slash + 1));
}
// each chart its own atlas name (M=...), the tag a plotter tells charts on one card apart by; a typed name is kept
static void AutoName(const std::wstring& path) {
  const std::wstring desc = GetText(gDesc);
  if (path.empty() || !(desc.empty() || desc == gAutoDesc)) return;
  std::wstring t;
  for (wchar_t c : Stem(path)) if (c < 128 && (iswalnum(c) || c == L'-' || c == L'_')) t += c;
  if (t.size() > 24) t.resize(24);
  gAutoDesc = L"M=" + (t.empty() ? std::wstring(L"SatMap") : t);
  SetWindowTextW(gDesc, gAutoDesc.c_str());
}
static void SetInput(const std::wstring& path) {
  SetWindowTextW(gOut, L"");
  SetWindowTextW(gIn, path.c_str());   // EN_CHANGE: name + kind
  size_t slash = path.find_last_of(L"\\/");
  std::wstring dir = slash == std::wstring::npos ? L"." : path.substr(0, slash);
  SetWindowTextW(gOut, (dir + L"\\" + Stem(path) + L"_AT5").c_str());
}
static std::wstring BrowseFile(bool folder) {
  std::wstring result;
  IFileOpenDialog* d = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return result;
  DWORD opt; d->GetOptions(&opt);
  d->SetOptions(opt | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
  if (!folder) {
    COMDLG_FILTERSPEC f[] = {{L"Map files (*.mbtiles;*.tif;*.tiff;*.jpg;*.jpeg;*.png)", L"*.mbtiles;*.tif;*.tiff;*.jpg;*.jpeg;*.png"},
                             {L"Coastline shapefiles (*.shp)", L"*.shp"}, {L"All files", L"*.*"}};
    d->SetFileTypes(3, f);
  }
  if (SUCCEEDED(d->Show(gWnd))) {
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->GetResult(&it))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = p; CoTaskMemFree(p); }
      it->Release();
    }
  }
  d->Release();
  return result;
}
static bool HasAt5(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE hf = FindFirstFileW((dir + L"\\*.at5").c_str(), &fd);
  if (hf == INVALID_HANDLE_VALUE) return false;
  FindClose(hf);
  return true;
}

// ---------------------------------------------------------------- running the converter
static void SetStatus(const std::wstring& s, COLORREF c) {
  gStatusColor = c;
  SetWindowTextW(gStatus, s.c_str());
  InvalidateRect(gStatus, nullptr, TRUE);
}
// what the converter is doing, in plain words, from one of its message lines ("" = no change)
static std::wstring Stage(const std::wstring& line) {
  auto has = [&](const wchar_t* t) { return line.find(t) != std::wstring::npos; };
  if (has(L"Source:") || has(L"Vector MBTiles:") || has(L"Levels:")) return L"Reading the map";
  if (has(L"Coastline:")) return L"Loading the coastline";
  if (has(L"Contour values") || has(L"Depth areas:")) return L"Looking at the depths";
  if (has(L"rendered") && has(L"tiles")) {
    const size_t p = line.find(L"rendered ") + 9;
    return L"Drawing map tiles (" + line.substr(p, line.find(L' ', p) - p) + L" so far)";
  }
  if (has(L"blocks ") && has(L"/")) {   // "blocks 2/5, tiles read ..." (several progress updates may share a line)
    const size_t p = line.rfind(L"blocks ") + 7, q = line.find(L'/', p);
    const size_t e = line.find_first_not_of(L"0123456789", q + 1);
    if (q != std::wstring::npos)
      return L"Building the chart (part " + line.substr(p, q - p) + L" of " + line.substr(q + 1, (e == std::wstring::npos ? line.size() : e) - q - 1) + L")";
  }
  if (has(L"Overview chart")) return L"Building the zoomed-out overview";
  if (has(L"Detail chart")) return L"Building the chart";
  if (has(L"Wrote") || has(L"Writing")) return L"Writing the chart files";
  if (has(L"preview ")) return L"Drawing the preview";
  return L"";
}
static std::wstring gLogTail;   // the start of a message line still arriving
static void AppendLog(const std::wstring& text) {
  int len = GetWindowTextLengthW(gLog);
  SendMessageW(gLog, EM_SETSEL, len, len);
  SendMessageW(gLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
  gLogTail += text;
  size_t a = 0, b;
  while ((b = gLogTail.find_first_of(L"\r\n", a)) != std::wstring::npos) {   // complete lines only
    const std::wstring st = Stage(gLogTail.substr(a, b - a));
    if (!st.empty()) gLastLine = st;
    a = b + 1;
  }
  gLogTail.erase(0, a);
}
static std::wstring Elapsed() {
  const ULONGLONG s = (GetTickCount64() - gStart) / 1000;
  wchar_t b[32]; swprintf_s(b, L"%llu:%02llu", s / 60, s % 60);
  return b;
}
static void SetProgress(int mode) {   // 0 idle (hidden), 1 running, 2 done, 3 failed
  ShowWindow(gProg, mode ? SW_SHOWNA : SW_HIDE);
  LONG_PTR st = GetWindowLongPtrW(gProg, GWL_STYLE);
  if (mode == 1) {
    SetWindowLongPtrW(gProg, GWL_STYLE, st | PBS_MARQUEE);
    SendMessageW(gProg, PBM_SETSTATE, PBST_NORMAL, 0);
    SendMessageW(gProg, PBM_SETMARQUEE, TRUE, 30);
  } else {
    SendMessageW(gProg, PBM_SETMARQUEE, FALSE, 0);
    SetWindowLongPtrW(gProg, GWL_STYLE, st & ~LONG_PTR(PBS_MARQUEE));
    SendMessageW(gProg, PBM_SETRANGE32, 0, 100);
    SendMessageW(gProg, PBM_SETPOS, mode >= 2 ? 100 : 0, 0);
    SendMessageW(gProg, PBM_SETSTATE, mode == 3 ? PBST_ERROR : PBST_NORMAL, 0);
  }
}
static void SetBusy(bool b, int task) {
  gBusy = b; gTask = task;
  EnableWindow(gConvert, !b || task == 0);
  for (HWND h : {gPreview, gVerify, gInBrowse, gOutBrowse, gIn, gOut}) EnableWindow(h, !b);
  InvalidateRect(gConvert, nullptr, TRUE);
  if (b) {
    gStart = GetTickCount64(); gLastLine.clear(); gLogTail.clear(); gStopped = false;
    SetProgress(1);
    SetTimer(gWnd, TIMER_TICK, 1000, nullptr);
  } else KillTimer(gWnd, TIMER_TICK);
}
static void OnTick() {
  static const wchar_t* verb[] = {L"Converting", L"Drawing the preview", L"Checking the files"};
  std::wstring s = std::wstring(verb[gTask]) + L"\x2026  " + Elapsed();
  if (!gLastLine.empty()) s += L"   \x00B7   " + gLastLine;
  SetStatus(s, kText);
}

// run sat2at5.exe with args, stream its output to the log (worker thread)
static void RunTool(std::wstring args) {
  std::wstring exe = EngineExe();
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    PostMessageW(gWnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(L"sat2at5.exe was not found. Keep the whole AT5 Chart Maker folder together:\r\n  " + exe + L"\r\n"));
    PostMessageW(gWnd, WM_APP_DONE, 2, 0);
    return;
  }
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE rd, wr;
  CreatePipe(&rd, &wr, &sa, 0);
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = Q(exe) + L" " + args;
  PostMessageW(gWnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(L"> sat2at5 " + args + L"\r\n"));
  if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
    CloseHandle(rd); CloseHandle(wr);
    PostMessageW(gWnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(L"could not start sat2at5.exe\r\n"));
    PostMessageW(gWnd, WM_APP_DONE, 2, 0);
    return;
  }
  if (gJob) AssignProcessToJobObject(gJob, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(wr);
  char buf[4096]; DWORD n; std::string pending;
  while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
    pending.append(buf, n);
    size_t keep = pending.size();   // a UTF-8 character cut at the end of this read waits for the next one
    while (keep > 0 && (uint8_t(pending[keep - 1]) & 0xC0) == 0x80) --keep;
    if (keep > 0 && uint8_t(pending[keep - 1]) >= 0xC0) --keep;
    std::string out;
    for (size_t i = 0; i < keep; ++i) {
      const char c = pending[i];
      if (c == '\r') { out += "\r\n"; continue; }        // progress lines -> new lines
      if (c == '\n') { if (out.empty() || out.back() != '\n') out += "\r\n"; continue; }
      out += c;
    }
    pending.erase(0, keep);
    int wl = MultiByteToWideChar(CP_UTF8, 0, out.c_str(), int(out.size()), nullptr, 0);
    auto* w = new std::wstring(size_t(wl), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, out.c_str(), int(out.size()), &(*w)[0], wl);
    PostMessageW(gWnd, WM_APP_LOG, 0, (LPARAM)w);
  }
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  PostMessageW(gWnd, WM_APP_DONE, code, 0);
}
static void Start(const std::wstring& args, int task) {
  SetWindowTextW(gLog, L"");
  SetBusy(true, task);
  OnTick();
  std::thread(RunTool, args).detach();
}

// stops a running conversion and removes its half-written work files
static void StopRun() {
  if (!gBusy || !gJob) return;
  gStopped = true;
  TerminateJobObject(gJob, 1);
  Sleep(300);
  const std::wstring out = GetText(gOut), tmp = out + L"\\_sat2at5_tmp";
  WIN32_FIND_DATAW fd;
  HANDLE hf = FindFirstFileW((tmp + L"\\*").c_str(), &fd);
  if (hf != INVALID_HANDLE_VALUE) {
    do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((tmp + L"\\" + fd.cFileName).c_str());
    while (FindNextFileW(hf, &fd));
    FindClose(hf);
  }
  RemoveDirectoryW(tmp.c_str());
  DeleteFileW((out + L"\\_sat2at5_vec_store.bin").c_str());
  RemoveDirectoryW(out.c_str());   // only succeeds when nothing else is in it
}

static void OnConvert() {
  if (gBusy) {
    if (gTask == 0 && AskBox(L"Stop the conversion?", L"The chart isn't finished. Its half-written files are removed.", L"Stop", L"Keep going"))
      StopRun();
    return;
  }
  const std::wstring in = Trim(GetText(gIn)), out = Trim(GetText(gOut));
  if (in.empty() || GetFileAttributesW(in.c_str()) == INVALID_FILE_ATTRIBUTES) {
    InfoBox(L"Choose a map file first", L"Drag your map file onto the window, or press Browse next to Map file.");
    return;
  }
  if (out.empty()) { InfoBox(L"Choose where to save the chart", L"Press Change next to Save to and pick a folder."); return; }
  std::wstring project;   // IMC Project Based Naming: the chart name (without M=) in every file name
  if (SendMessageW(gProject, BM_GETCHECK, 0, 0) == BST_CHECKED) {
    const std::wstring d = GetText(gDesc), pj = d.size() > 2 && d[1] == L'=' ? d.substr(2) : d;
    for (wchar_t c : pj) if (c < 128 && (iswalnum(c) || c == L'-')) project += c;
    if (project.size() > 24) project.resize(24);
    if (project.find_first_not_of(L"0123456789-") == std::wstring::npos) {
      InfoBox(L"The chart needs a name", L"Give the chart a name with letters in it, to use in the file names.");
      return;
    }
  }
  {  // .at5 files from an earlier chart left in the folder would mix into this one on the card
    std::vector<std::wstring> old;
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((out + L"\\*.at5").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
      // with project naming, other charts' files may share the folder: only this chart's count
      do if (project.empty() || Lower(fd.cFileName).find(Lower(project)) != std::wstring::npos) old.push_back(fd.cFileName);
      while (FindNextFileW(hf, &fd));
      FindClose(hf);
    }
    if (!old.empty()) {
      if (!AskBox(L"Replace the chart in this folder?",
                  L"The Save to folder already has " + std::to_wstring(old.size()) + L" chart file(s) from an earlier conversion:\n" +
                      out + L"\n\nThey are deleted first, so old and new files don't mix on the SD card.",
                  L"Replace them", L"Cancel"))
        return;
      for (auto& n : old) DeleteFileW((out + L"\\" + n).c_str());
    }
  }
  std::wstring args = Q(in) + L" -o " + Q(out);
  const std::wstring desc = Trim(GetText(gDesc));
  if (!desc.empty()) args += L" --desc " + Q(desc);
  if (!project.empty()) args += L" --project " + Q(project);
  if (gKindCode == 3) {   // depth survey
    static const wchar_t* palettes[] = {L"spectrum", L"lagoon", L"thermal", L"chart", L"grey"};
    static const wchar_t* shades[] = {L"0", L"30", L"60", L"90"};
    const int pi = int(SendMessageW(gPalette, CB_GETCURSEL, 0, 0)), si = int(SendMessageW(gShade, CB_GETCURSEL, 0, 0));
    args += std::wstring(L" --palette ") + palettes[std::clamp(pi, 0, 4)];
    args += std::wstring(L" --hillshade ") + shades[std::clamp(si, 0, 3)];
    static const wchar_t* finestD[] = {nullptr, L"0.5", L"1", L"2", L"4", L"8", L"16"};
    const int fd = int(SendMessageW(gFinest, CB_GETCURSEL, 0, 0));
    if (fd > 0 && fd < 7) args += std::wstring(L" --finest ") + finestD[fd];
    std::wstring range;
    for (wchar_t c : Trim(GetText(gRange))) if (c != L' ' && c != L'm') range += c;
    if (!range.empty()) {
      if (range.find(L'-', 1) == std::wstring::npos) range = L"0-" + range;   // "200" means 0-200
      args += L" --depth-range " + Q(range);
    }
    if (SendMessageW(gContours, BM_GETCHECK, 0, 0) == BST_CHECKED) {
      std::wstring every;
      for (wchar_t c : Trim(GetText(gInterval))) if (c != L' ' && c != L'm') every += c;
      args += L" --contours " + Q(every.empty() ? std::wstring(L"auto") : every);
      const int landSel = int(SendMessageW(gLand, CB_GETCURSEL, 0, 0));
      std::wstring land = Trim(GetText(gLand));
      if (landSel == 0) land = L"auto";
      else if (landSel == 1) land.clear();
      if (!land.empty()) args += L" --land " + Q(land);
    }
  } else if (gKindCode == 1) {   // picture
    static const wchar_t* finest[] = {nullptr, L"0.5", L"1", L"2", L"4", L"8", L"16"};
    const int fi = int(SendMessageW(gFinest, CB_GETCURSEL, 0, 0));
    if (fi > 0 && fi < 7) args += std::wstring(L" --finest ") + finest[fi];
  } else {               // depth chart
    const int style = int(SendMessageW(gScheme, CB_GETCURSEL, 0, 0));
    const std::wstring name = Trim(GetText(gName));
    const std::wstring bands = style == 0 ? L"navionics" : style == 1 ? std::wstring(kBandsS52) : style == 2 ? L"gradient" : Trim(GetText(gBands));
    if (bands.empty()) { InfoBox(L"Custom colours are empty", L"Fill in Custom colours under More settings, or pick another colour style."); return; }
    if (!name.empty() && name != L"Large") args += L" --name " + Q(name);
    if (!bands.empty()) args += L" --colours " + Q(bands);
    const std::wstring deep = Trim(GetText(gDeep)), safety = Trim(GetText(gSafety)), hilite = Trim(GetText(gHilite));
    if ((bands == L"gradient" || bands == L"navionics") && !deep.empty()) args += L" --deep-depth " + Q(deep);
    if (!safety.empty()) args += L" --safety-depth " + Q(safety);
    if (!hilite.empty()) args += L" --highlight " + Q(hilite);
    const int landSel = int(SendMessageW(gLand, CB_GETCURSEL, 0, 0));
    std::wstring land = Trim(GetText(gLand));
    const std::wstring drying = Trim(GetText(gDrying));
    if (landSel == 0) land = L"auto";
    else if (landSel == 1) land.clear();
    if (!land.empty()) args += L" --land " + Q(land);
    if (!land.empty() && !drying.empty() && drying != L"1.5") args += L" --drying-height " + Q(drying);
    if (SendMessageW(gOverview, BM_GETCHECK, 0, 0) != BST_CHECKED) args += L" --no-overview";
    const int sy = int(SendMessageW(gSynth, CB_GETCURSEL, 0, 0));
    if (sy == 1) args += L" --areas-from-contours";
    else if (sy == 2) args += L" --no-areas-from-contours";
    const std::wstring major = Trim(GetText(gMajor));
    if (!major.empty() && major != L"5") args += L" --major-contours " + Q(major);
    static const wchar_t* fine[] = {L"close", L"medium", L"always", L"never"};
    const int fi2 = int(SendMessageW(gFine, CB_GETCURSEL, 0, 0));
    if (fi2 > 0 && fi2 < 4) args += std::wstring(L" --fine-contours ") + fine[fi2];
  }
  Start(args, 0);
}
static void OnPreview() {
  const std::wstring out = Trim(GetText(gOut));
  if (!HasAt5(out)) { InfoBox(L"Nothing to preview yet", L"Press Convert first. Preview shows a picture of the finished chart."); return; }
  gPreviewPng = out + L"\\_preview.png";
  const bool lines = SendMessageW(gPvLines, BM_GETCHECK, 0, 0) == BST_CHECKED;
  Start(L"preview " + Q(out) + L" " + Q(gPreviewPng) + L" 2000" + (lines ? L"" : L" --no-lines"), 1);
}
static void OnVerify() {
  const std::wstring out = Trim(GetText(gOut));
  if (!HasAt5(out)) { InfoBox(L"Nothing to check yet", L"Press Convert first. Check files tests the finished chart files."); return; }
  Start(L"verify " + Q(out), 2);
}
static void OnDone(DWORD code) {
  const int task = gTask;
  SetBusy(false, task);
  const std::wstring t = Elapsed();
  if (gStopped) { SetProgress(0); SetStatus(L"Stopped. Nothing was saved.", kMuted); return; }
  SetProgress(code == 0 ? 2 : 3);
  if (code != 0) {
    SetStatus(L"\x2715  That didn't work \x2013 the messages below say why.", kWarn);
    return;
  }
  if (task == 0) {
    std::wstring out = Trim(GetText(gOut));
    while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    const std::wstring folder = out.substr(out.find_last_of(L"\\/") + 1);
    SetStatus(L"\x2713  Done in " + t + L".  Copy the folder \x201C" + folder + L"\x201D onto your SD card.", kOk);
  } else if (task == 1) {
    SetStatus(L"\x2713  Preview opened.", kOk);
    if (!gPreviewPng.empty()) ShellExecuteW(gWnd, L"open", gPreviewPng.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  } else {
    SetStatus(L"\x2713  All chart files are valid.", kOk);
  }
  gPreviewPng.clear();
}

// ---------------------------------------------------------------- window
static void CreateControls() {
  gTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                         CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, gWnd, nullptr, nullptr, nullptr);
  SendMessageW(gTip, TTM_SETMAXTIPWIDTH, 0, S(320));
  gGuide = Btn(L"How to use", ID_GUIDE, BS_OWNERDRAW);
  gAbout = Btn(L"About", ID_ABOUT, BS_OWNERDRAW);
  // 1  Your map
  lMap = Label(L"Map file");
  gIn = EditBox(ID_IN, L"Drop a map file here, or press Browse");
  gInBrowse = Btn(L"Browse\x2026", ID_IN_BROWSE);
  HelpBtn(H_MAP);
  gKind = Label(L"");
  lOut = Label(L"Save to");
  gOut = EditBox(ID_OUT, L"Fills in when you choose a map");
  gOutBrowse = Btn(L"Change\x2026", ID_OUT_BROWSE);
  HelpBtn(H_OUT);
  lName = Label(L"Chart name");
  gDesc = EditBox(ID_DESC, L"Fills in when you choose a map");
  HelpBtn(H_NAME);
  gProject = Check(L"Also put the name in the file names", ID_PROJECT, false);
  HelpBtn(H_PROJECT);
  // 2  Chart look (depth charts)
  lStyle = Label(L"Colour style");
  gScheme = Combo(ID_SCHEME, {L"Navionics style (recommended)", L"Paper chart (S-52)", L"Soft blue", L"Custom"}, 0);
  HelpBtn(H_STYLE);
  lDeep = Label(L"White from"); gDeep = EditBox(ID_DEEP); SetWindowTextW(gDeep, L"20"); lDeepM = Label(L"m");
  HelpBtn(H_DEEP);
  lLand = Label(L"Coastline");
  gLand = Combo(ID_LAND, {kLandAuto, kLandNone}, 0);
  gLandBrowse = Btn(L"Browse\x2026", ID_LAND_BROWSE);
  HelpBtn(H_LAND);
  lDry = Label(L"Drying strip"); gDrying = EditBox(ID_DRYING); SetWindowTextW(gDrying, L"1.5"); lDryM = Label(L"m");
  HelpBtn(H_DRYING);
  lSafe = Label(L"Safety depth"); gSafety = EditBox(ID_SAFETY, L"none"); lSafeM = Label(L"m");
  HelpBtn(H_SAFETY);
  lHi = Label(L"Highlight"); gHilite = EditBox(ID_HILITE, L"e.g. 10-20");
  HelpBtn(H_HILITE);
  gMore = Btn(L"More settings  \x25B8", ID_MORE, BS_OWNERDRAW);
  lMajor = Label(L"Zoomed-out contours every"); gMajor = EditBox(ID_MAJOR); SetWindowTextW(gMajor, L"5"); lMajorM = Label(L"m");
  HelpBtn(H_MAJOR);
  lFine = Label(L"Finer contours appear");
  gFine = Combo(ID_FINE, {L"When zoomed in close", L"From medium zoom", L"Always", L"Never"}, 0);
  HelpBtn(H_FINE);
  lSynth = Label(L"Colour between contours");
  gSynth = Combo(ID_SYNTH, {L"Automatic (when the file has no depth areas)", L"Always", L"Never (file's own areas only)"}, 0);
  HelpBtn(H_SYNTH);
  lBands = Label(L"Custom colours");
  gBands = EditBox(ID_BANDS, L"e.g. min:83B295, 0:73B6EF, 5:BAD5E1, 10:D4EAEE");
  EnableWindow(gBands, FALSE);
  HelpBtn(H_BANDS);
  gOverview = Check(L"Also make a zoomed-out overview file", ID_OVERVIEW, true);
  HelpBtn(H_OVERVIEW);
  lVName = Label(L"Detail file name"); gName = EditBox(ID_NAME); SetWindowTextW(gName, L"Large");
  HelpBtn(H_VNAME);
  gPvLines = Check(L"Show contour lines in Preview", ID_PVLINES, false);
  HelpBtn(H_PVLINES);
  // 2  Chart look (pictures)
  lFinest = Label(L"Detail");
  gFinest = Combo(ID_FINEST, {L"Auto (recommended)", L"0.5 m per pixel", L"1 m per pixel", L"2 m per pixel", L"4 m per pixel",
                              L"8 m per pixel", L"16 m per pixel"}, 0);
  HelpBtn(H_FINEST);
  gPicNote = Label(L"Picture maps keep their own colours. The plotter shows them as its imagery / shaded-relief layer, "
                   L"so turn that overlay on in the plotter's chart settings.");
  // 2  Chart look (depth surveys)
  lPalette = Label(L"Colours");
  gPalette = Combo(ID_PALETTE, {L"Spectrum (red shallow, blue deep)", L"Lagoon", L"Thermal", L"Chart blues", L"Grey"}, 0);
  HelpBtn(H_PALETTE);
  lRange = Label(L"Depth range"); gRange = EditBox(ID_RANGE, L"auto"); lRangeM = Label(L"m");
  HelpBtn(H_RANGE);
  lShade = Label(L"Hillshade");
  gShade = Combo(ID_SHADE, {L"Off", L"Light", L"Medium (recommended)", L"Strong"}, 2);
  HelpBtn(H_SHADE);
  gContours = Check(L"Also make contour lines every", ID_CONTOURS, true);
  gInterval = EditBox(ID_INTERVAL, L"auto"); lIntervalM = Label(L"m");
  HelpBtn(H_CONTOURS);
  gBathyNote = Label(L"The picture becomes the plotter's imagery / shaded-relief layer and the contour lines a chart "
                     L"on top, each switched on and off in the plotter's chart settings.");
  // 3  Make the chart
  gConvert = Btn(L"Convert", ID_CONVERT, BS_OWNERDRAW);
  gPreview = Btn(L"Preview", ID_PREVIEW);
  gVerify = Btn(L"Check files", ID_VERIFY);
  gOpen = Btn(L"Open folder", ID_OPEN);
  AddTip(gPreview, L"Opens a picture of the finished chart.");
  AddTip(gVerify, L"Tests the finished chart files. It should find no problems.");
  AddTip(gOpen, L"Shows the chart files. Copy this whole folder onto the SD card.");
  gProg = Make(PROGRESS_CLASSW, L"", 0, 0);
  gStatus = Label(L"Ready.");
  gLog = Make(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, ID_LOG, WS_EX_CLIENTEDGE);
  SendMessageW(gLog, EM_SETLIMITTEXT, 0, 0);
  SetWindowTextW(gLog, L"Messages from the converter appear here.\r\n\r\nNew here? Click \"How to use\" at the top right for a short walkthrough. Every \"?\" explains the setting beside it.");
  EnumChildWindows(gWnd, SetFontProc, 0);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: {
      gWnd = h;
      gDpi = GetDpiForWindow(h);
      MakeFonts();
      CreateControls();
      gStatusColor = kMuted;
      SetProgress(0);
      DetectKind();
      DragAcceptFiles(h, TRUE);
      SendMessageW(h, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
      PostMessageW(h, WM_APP_KIND + 1, 0, 0);   // after the window is shown: fit its height, focus the map box
      return 0;
    }
    case WM_DPICHANGED: {
      gDpi = HIWORD(w);
      MakeFonts();
      EnumChildWindows(h, SetFontProc, 0);
      const RECT* r = (const RECT*)l;
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      Relayout();
      return 0;
    }
    case WM_SIZE: gMinH = Layout(LOWORD(l), HIWORD(l)); return 0;
    case WM_GETMINMAXINFO: {
      RECT r{0, 0, S(kLeftW + 16 + 12 + 300 + 16), gMinH};
      AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, gDpi);
      MONITORINFO mi{sizeof mi}; GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
      ((MINMAXINFO*)l)->ptMinTrackSize = {r.right - r.left, std::min<LONG>(r.bottom - r.top, mi.rcWork.bottom - mi.rcWork.top)};
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
      RECT rc; GetClientRect(h, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ ob = SelectObject(mem, bm);
      Paint(mem, rc);
      BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = (HDC)w; HWND c = (HWND)l;
      SetBkColor(dc, kCard);
      COLORREF col = kText;
      if (c == gKind) col = gKindColor;
      else if (c == gStatus) col = gStatusColor;
      else if (c == gPicNote || c == lDeepM || c == lDryM || c == lSafeM || c == lMajorM) col = kMuted;
      SetTextColor(dc, col);
      return (LRESULT)gCardBrush;
    }
    case WM_DRAWITEM: DrawOwnerButton((const DRAWITEMSTRUCT*)l); return TRUE;
    case WM_SETCURSOR:
      if (IsLink((HWND)w) || IsHelp((HWND)w) || ((HWND)w == gConvert && IsWindowEnabled(gConvert))) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_DROPFILES: {
      wchar_t p[MAX_PATH];
      if (!gBusy && DragQueryFileW((HDROP)w, 0, p, MAX_PATH)) {
        DWORD a = GetFileAttributesW(p);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) SetWindowTextW(gOut, p);
        else SetInput(p);
      }
      DragFinish((HDROP)w);
      return 0;
    }
    case WM_TIMER:
      if (w == TIMER_KIND) { KillTimer(h, TIMER_KIND); DetectKind(); }
      else if (w == TIMER_TICK && gBusy) OnTick();
      return 0;
    case WM_COMMAND: {
      const int id = LOWORD(w);
      if (id >= ID_HELP0 && id < ID_HELP0 + H_COUNT) { InfoBox(kHelp[id - ID_HELP0].title, kHelp[id - ID_HELP0].text); return 0; }
      switch (id) {
        case ID_GUIDE: ShowGuide(); break;
        case ID_ABOUT: ShowAbout(); break;
        case ID_IN_BROWSE: { auto p = BrowseFile(false); if (!p.empty()) SetInput(p); break; }
        case ID_OUT_BROWSE: { auto p = BrowseFile(true); if (!p.empty()) SetWindowTextW(gOut, p.c_str()); break; }
        case ID_LAND_BROWSE: {   // the chosen coastline file becomes the third entry of the list
          auto p = BrowseFile(false);
          if (p.empty()) break;
          while (SendMessageW(gLand, CB_GETCOUNT, 0, 0) > 2) SendMessageW(gLand, CB_DELETESTRING, 2, 0);
          SendMessageW(gLand, CB_ADDSTRING, 0, (LPARAM)p.c_str());
          SendMessageW(gLand, CB_SETCURSEL, 2, 0);
          break;
        }
        case ID_IN:
          if (HIWORD(w) == EN_CHANGE) { AutoName(Trim(GetText(gIn))); SetTimer(h, TIMER_KIND, 400, nullptr); }
          break;
        case ID_CONVERT: OnConvert(); break;
        case ID_PREVIEW: if (!gBusy) OnPreview(); break;
        case ID_VERIFY: if (!gBusy) OnVerify(); break;
        case ID_OPEN: {
          const std::wstring o = Trim(GetText(gOut));
          if (!o.empty() && GetFileAttributesW(o.c_str()) != INVALID_FILE_ATTRIBUTES) ShellExecuteW(h, L"open", o.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
          else InfoBox(L"No folder yet", L"The Save to folder is made when you press Convert.");
          break;
        }
        case ID_CONTOURS: {   // contour lines on/off: the spacing and coastline only matter with lines
          const BOOL on = SendMessageW(gContours, BM_GETCHECK, 0, 0) == BST_CHECKED;
          EnableWindow(gInterval, on);
          Relayout(true);
          break;
        }
        case ID_MORE:
          gAdvanced = !gAdvanced;
          SetWindowTextW(gMore, gAdvanced ? L"Fewer settings  \x25BE" : L"More settings  \x25B8");
          Relayout(true);
          break;
        case ID_SCHEME:
          if (HIWORD(w) == CBN_SELCHANGE) {
            const int sel = int(SendMessageW(gScheme, CB_GETCURSEL, 0, 0));
            EnableWindow(gDeep, sel == 0 || sel == 2);
            EnableWindow(gBands, sel == 3);
            if (sel == 3) {
              if (!gAdvanced) SendMessageW(h, WM_COMMAND, ID_MORE, 0);
              if (Trim(GetText(gBands)).empty()) SetWindowTextW(gBands, kBandsS52);
              SetFocus(gBands);
              SendMessageW(gBands, EM_SETSEL, 0, -1);
            }
          }
          break;
      }
      return 0;
    }
    case WM_APP_LOG: { auto* s = (std::wstring*)l; AppendLog(*s); delete s; return 0; }
    case WM_APP_DONE: OnDone(DWORD(w)); return 0;
    case WM_APP_KIND: OnKind(int(w)); return 0;
    case WM_APP_KIND + 1:
      Relayout(true);
      SetFocus(gIn);
      return 0;
    case WM_CLOSE:
      if (gBusy) {
        if (!AskBox(L"A conversion is still running", L"Quit anyway? The conversion is stopped and its half-written files are removed.", L"Quit", L"Keep going"))
          return 0;
        StopRun();
      }
      DestroyWindow(h);
      return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  gJob = CreateJobObjectW(nullptr, nullptr);   // the converter ends with this program, even if it is killed
  if (gJob) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(gJob, JobObjectExtendedLimitInformation, &li, sizeof li);
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX ic{sizeof ic, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES}; InitCommonControlsEx(&ic);
  gBgBrush = CreateSolidBrush(kBg);
  gCardBrush = CreateSolidBrush(kCard);
  gIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR);
  gIconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
  if (!gIcon) gIcon = LoadIcon(nullptr, IDI_APPLICATION);
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"AT5ChartMaker";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = nullptr;
  wc.hIcon = gIcon; wc.hIconSm = gIconSmall ? gIconSmall : gIcon;
  RegisterClassExW(&wc);
  // 1080 x 660 in this monitor's scale, kept inside the screen's work area
  const UINT dpi = GetDpiForSystem();
  RECT r{0, 0, MulDiv(1080, int(dpi), 96), MulDiv(660, int(dpi), 96)};
  AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
  RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  r.right = r.left + std::min(r.right - r.left, LONG(wa.right - wa.left));
  r.bottom = r.top + std::min(r.bottom - r.top, LONG(wa.bottom - wa.top));
  HWND h = CreateWindowExW(0, L"AT5ChartMaker", kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  ShowWindow(h, show);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  }
  return 0;
}
