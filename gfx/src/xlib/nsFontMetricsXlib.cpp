/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#include "xp_core.h"
#include "nsQuickSort.h"
#include "nsFontMetricsXlib.h"
#include "nsIServiceManager.h"
#include "nsICharsetConverterManager.h"
#include "nsICharRepresentable.h"
#include "nsCOMPtr.h"
#include "nspr.h"
#include "plhash.h"

#include <X11/Xatom.h>
#include <stdlib.h>

// these are in the widget set

extern Display         *gDisplay;

//#undef NOISY_FONTS
//#undef REALLY_NOISY_FONTS

#ifdef DEBUG_blizzard
#define NOISY_FONTS
#define REALLY_NOISY_FONTS
#endif

static NS_DEFINE_IID(kIFontMetricsIID, NS_IFONT_METRICS_IID);

nsFontMetricsXlib::nsFontMetricsXlib()
{
  NS_INIT_REFCNT();
  mDeviceContext = nsnull;
  mFont = nsnull;
  mFontHandle = nsnull;

  mHeight = 0;
  mAscent = 0;
  mDescent = 0;
  mLeading = 0;
  mMaxAscent = 0;
  mMaxDescent = 0;
  mMaxAdvance = 0;
  mXHeight = 0;
  mSuperscriptOffset = 0;
  mSubscriptOffset = 0;
  mStrikeoutSize = 0;
  mStrikeoutOffset = 0;
  mUnderlineSize = 0;
  mUnderlineOffset = 0;
  mSpaceWidth = 0;
}

nsFontMetricsXlib::~nsFontMetricsXlib()
{
  if (nsnull != mFont) {
    delete mFont;
    mFont = nsnull;
  }

#ifdef FONT_SWITCHING

  if (mFonts) {
    delete [] mFonts;
    mFonts = nsnull;
  }

  if (mLoadedFonts) {
    PR_Free(mLoadedFonts);
    mLoadedFonts = nsnull;
  }

  mFontHandle = 0;

#else

  if (0 != mFontHandle) {
    XFreeFont(gDisplay, mFontHandle);
  }

#endif

}

NS_IMPL_ISUPPORTS(nsFontMetricsXlib, kIFontMetricsIID)

#ifdef FONT_SWITCHING

static PRBool
FontEnumCallback(const nsString& aFamily, PRBool aGeneric, void *aData)
{
  nsFontMetricsXlib* metrics = (nsFontMetricsXlib*) aData;
  if (metrics->mFontsCount == metrics->mFontsAlloc) {
    int newSize = 2 * (metrics->mFontsAlloc ? metrics->mFontsAlloc : 1);
    nsString* newPointer = new nsString[newSize];
    if (newPointer) {
      for (int i = metrics->mFontsCount - 1; i >= 0; i--) {
        newPointer[i].SetString(metrics->mFonts[i].GetUnicode());
      }
      delete [] metrics->mFonts;
      metrics->mFonts = newPointer;
      metrics->mFontsAlloc = newSize;
    }
    else {
      return PR_FALSE;
    }
  }
  metrics->mFonts[metrics->mFontsCount].SetString(aFamily.GetUnicode());
  metrics->mFonts[metrics->mFontsCount++].ToLowerCase();

  return PR_TRUE;
}

#endif /* FONT_SWITCHING */

NS_IMETHODIMP nsFontMetricsXlib::Init(const nsFont& aFont, nsIDeviceContext* aContext)
{
  NS_ASSERTION(!(nsnull == aContext), "attempt to init fontmetrics with null device context");

#ifdef FONT_SWITCHING

  mFont = new nsFont(aFont);

  mDeviceContext = aContext;

  float app2dev;
  mDeviceContext->GetAppUnitsToDevUnits(app2dev);
  char* factorStr = getenv("GECKO_FONT_SIZE_FACTOR");
  double factor;
  if (factorStr) {
    factor = atof(factorStr);
  }
  else {
    factor = 1.0;
  }
  mPixelSize = NSToIntRound(app2dev * factor * mFont->size);
  mStretchIndex = 4; // normal
  mStyleIndex = mFont->style;

  mFont->EnumerateFamilies(FontEnumCallback, this);

  nsFontXlib* f = FindFont('a');
  if (!f) {
    return NS_OK; // XXX
  }
  mFontHandle = f->mFont;

  RealizeFont();

#else /* FONT_SWITCHING */

  nsAutoString  firstFace;
  if (NS_OK != aContext->FirstExistingFont(aFont, firstFace)) {
    aFont.GetFirstFamily(firstFace);
  }

  char        **fnames = nsnull;
  PRInt32     namelen = firstFace.Length() + 1;
  char	      *wildstring = (char *)PR_Malloc((namelen << 1) + 200);
  int         numnames = 0;
  char        altitalicization = 0;
  XFontStruct *fonts;
  float       t2d;
  aContext->GetTwipsToDevUnits(t2d);
  PRInt32     dpi = NSToIntRound(t2d * 1440);

  if (nsnull == wildstring)
    return NS_ERROR_NOT_INITIALIZED;

  mFont = new nsFont(aFont);
  mDeviceContext = aContext;
  mFontHandle = 0;
  mFontStruct = nsnull;

  firstFace.ToCString(wildstring, namelen);

  if (abs(dpi - 75) < abs(dpi - 100))
    dpi = 75;
  else
    dpi = 100;

#ifdef NOISY_FONTS
  printf("looking for font %s (%d)", wildstring, aFont.size / 20);
#endif

  //font properties we care about:
  //name
  //weight (bold, medium)
  //slant (r = normal, i = italic, o = oblique)
  //size in nscoords >> 1

  // XXX oddly enough, enabling font scaling *here* breaks the
  // text-field widgets...
  static PRBool allowFontScaling = PR_FALSE;
#ifdef DEBUG
  static PRBool firstTime = 1;
  if (firstTime) {
    char *gsf = getenv("GECKO_SCALE_FONTS");
    if (gsf)
    {
      allowFontScaling = PR_TRUE;
    }
  }
#endif
  if (allowFontScaling)
  {
    // Try 0,0 dpi first in case we have a scalable font
    PR_snprintf(&wildstring[namelen + 1], namelen + 200,
                "-*-%s-%s-%c-normal-*-*-%d-0-0-*-*-*-*",
                wildstring,
                (aFont.weight <= NS_FONT_WEIGHT_NORMAL) ? "medium" : "bold",
                ((aFont.style == NS_FONT_STYLE_NORMAL)
                 ? 'r'
                 : ((aFont.style == NS_FONT_STYLE_ITALIC) ? 'i' : 'o')),
                aFont.size / 2);
    fnames = ::XListFontsWithInfo(gDisplay, &wildstring[namelen + 1],
                                  200, &numnames, &fonts);
#ifdef NOISY_FONTS
    printf("  trying %s[%d]", &wildstring[namelen+1], numnames);
#endif
  }

  if (numnames <= 0)
  {
    // If no scalable font, then try using our dpi
    PR_snprintf(&wildstring[namelen + 1], namelen + 200,
                "-*-%s-%s-%c-normal-*-*-*-%d-%d-*-*-*-*",
                wildstring,
                (aFont.weight <= NS_FONT_WEIGHT_NORMAL) ? "medium" : "bold",
                (aFont.style == NS_FONT_STYLE_NORMAL) ? 'r' :
                ((aFont.style == NS_FONT_STYLE_ITALIC) ? 'i' : 'o'), dpi, dpi);
    fnames = ::XListFontsWithInfo(gDisplay, &wildstring[namelen + 1],
                                  200, &numnames, &fonts);
#ifdef NOISY_FONTS
    printf("  trying %s[%d]", &wildstring[namelen+1], numnames);
#endif

    if (aFont.style == NS_FONT_STYLE_ITALIC)
      altitalicization = 'o';
    else if (aFont.style == NS_FONT_STYLE_OBLIQUE)
      altitalicization = 'i';

    if ((numnames <= 0) && altitalicization)
    {
      PR_snprintf(&wildstring[namelen + 1], namelen + 200,
                  "-*-%s-%s-%c-normal-*-*-*-%d-%d-*-*-*-*",
                  wildstring,
                  (aFont.weight <= NS_FONT_WEIGHT_NORMAL) ? "medium" : "bold",
                  altitalicization, dpi, dpi);

      fnames = ::XListFontsWithInfo(gDisplay, &wildstring[namelen + 1],
                                    200, &numnames, &fonts);
#ifdef NOISY_FONTS
      printf("  trying %s[%d]", &wildstring[namelen+1], numnames);
#endif
    }


    if (numnames <= 0)
    {
      //we were not able to match the font name at all...

      char *newname = firstFace.ToNewCString();

      PR_snprintf(&wildstring[namelen + 1], namelen + 200,
                  "-*-%s-%s-%c-normal-*-*-*-%d-%d-*-*-*-*",
                  newname,
                  (aFont.weight <= NS_FONT_WEIGHT_NORMAL) ? "medium" : "bold",
                  (aFont.style == NS_FONT_STYLE_NORMAL) ? 'r' :
                  ((aFont.style == NS_FONT_STYLE_ITALIC) ? 'i' : 'o'),
                  dpi, dpi);
      fnames = ::XListFontsWithInfo(gDisplay, &wildstring[namelen + 1],
                                    200, &numnames, &fonts);
#ifdef NOISY_FONTS
      printf("  trying %s[%d]", &wildstring[namelen+1], numnames);
#endif

      if ((numnames <= 0) && altitalicization)
      {
        PR_snprintf(&wildstring[namelen + 1], namelen + 200,
                    "-*-%s-%s-%c-normal-*-*-*-%d-%d-*-*-*-*",
                    newname,
                    (aFont.weight <= NS_FONT_WEIGHT_NORMAL) ? "medium" : "bold",
                    altitalicization, dpi, dpi);
        fnames = ::XListFontsWithInfo(gDisplay, &wildstring[namelen + 1],
                                      200, &numnames, &fonts);
#ifdef NOISY_FONTS
        printf("  trying %s[%d]", &wildstring[namelen+1], numnames);
#endif
      }

      delete [] newname;
    }
  }

  if (numnames > 0)
  {
    char *nametouse = PickAppropriateSize(fnames, fonts, numnames, aFont.size);

    mFontStruct = XLoadQueryFont(gDisplay, nametouse);
    mFontHandle = mFontStruct->fid;

#ifdef NOISY_FONTS
    printf(" is: %s\n", nametouse);
#endif

    ::XFreeFontInfo(fnames, fonts, numnames);
  }
  else
  {
    //ack. we're in real trouble, go for fixed...

#ifdef NOISY_FONTS
    printf(" is: %s\n", "fixed (final fallback)");
#endif

    mFontStruct = XLoadQueryFont(gDisplay, "fixed");
    mFontHandle = mFontStruct->fid;
  }

  RealizeFont();

  PR_Free(wildstring);

#endif /* FONT_SWITCHING */

  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::Destroy()
{
//  NS_IF_RELEASE(mDeviceContext);
  return NS_OK;
}

char *
nsFontMetricsXlib::PickAppropriateSize(char **names, XFontStruct *fonts,
                                      int cnt, nscoord desired)
{
  int         idx;
  float       app2dev;
  mDeviceContext->GetAppUnitsToDevUnits(app2dev);
  PRInt32     desiredPix = NSToIntRound(app2dev * desired);
  XFontStruct *curfont;
  PRInt32     closestMin = -1, minIndex = 0;
  PRInt32     closestMax = 1<<30, maxIndex = 0;

  // Find exact match, closest smallest and closest largest. If the
  // largest is too much larger always pick the smallest.
  for (idx = 0, curfont = fonts; idx < cnt; idx++, curfont++)
  {
    PRInt32 height = curfont->ascent + curfont->descent;
    if (height == desiredPix) {
      // Winner. Found an *exact* match
      return names[idx];
    }

    if (height < desiredPix) {
      // If the height is closer to the desired height, remember this font
      if (height > closestMin) {
        closestMin = height;
        minIndex = idx;
      }
    }
    else {
      if (height < closestMax) {
        closestMax = height;
        maxIndex = idx;
      }
    }
  }

  // If the closest smaller font is closer than the closest larger
  // font, use it.
#ifdef NOISY_FONTS
  printf(" *** desiredPix=%d(%d) min=%d max=%d *** ",
         desiredPix, desired, closestMin, closestMax);
#endif
  if (desiredPix - closestMin <= closestMax - desiredPix) {
    return names[minIndex];
  }

  // If the closest larger font is more than 2 pixels too big, use the
  // closest smaller font. This is done to prevent things from being
  // way too large.
  if (closestMax - desiredPix > 2) {
    return names[minIndex];
  }
  return names[maxIndex];
}

void nsFontMetricsXlib::RealizeFont()
{

  float f;
  mDeviceContext->GetDevUnitsToAppUnits(f);

  mAscent = nscoord(mFontHandle->ascent * f);
  mDescent = nscoord(mFontHandle->descent * f);
  mMaxAscent = nscoord(mFontHandle->ascent * f) ;
  mMaxDescent = nscoord(mFontHandle->descent * f);

  mHeight = nscoord((mFontHandle->ascent + mFontHandle->descent) * f);
  mMaxAdvance = nscoord(mFontHandle->max_bounds.width * f);

  // 56% of ascent, best guess for non-true type
  mXHeight = NSToCoordRound((float) mFontHandle->ascent* f * 0.56f);

  int rawWidth = XTextWidth(mFontHandle, " ", 1);
  mSpaceWidth = NSToCoordRound(rawWidth * f);
  unsigned long pr = 0;

  if (::XGetFontProperty(mFontHandle, XA_X_HEIGHT, &pr))
  {
    mXHeight = nscoord(pr * f);
#ifdef REALLY_NOISY_FONTS
    printf("xHeight=%d\n", mXHeight);
#endif
  }

  if (::XGetFontProperty(mFontHandle, XA_UNDERLINE_POSITION, &pr))
  {
    /* this will only be provided from adobe .afm fonts */
    mUnderlineOffset = NSToIntRound(pr * f);
#ifdef REALLY_NOISY_FONTS
    printf("underlineOffset=%d\n", mUnderlineOffset);
#endif
  }
  else
  {
    /* this may need to be different than one for those weird asian fonts */
    /* mHeight is already multipled by f */
    float height;
    height = mFontHandle->ascent + mFontHandle->descent;
    mUnderlineOffset = -NSToIntRound(MAX (1, floor (0.1 * height + 0.5)) * f);
  }

  if (::XGetFontProperty(mFontHandle, XA_UNDERLINE_THICKNESS, &pr))
  {
    /* this will only be provided from adobe .afm fonts */
    mUnderlineSize = nscoord(MAX(f, NSToIntRound(pr * f)));
#ifdef REALLY_NOISY_FONTS
    printf("underlineSize=%d\n", mUnderlineSize);
#endif
  }
  else
  {
    /* mHeight is already multipled by f */
    float height;
    height = mFontHandle->ascent + mFontHandle->descent;
    mUnderlineSize = NSToIntRound(MAX(1, floor (0.05 * height + 0.5)) * f);
  }

  if (::XGetFontProperty(mFontHandle, XA_SUPERSCRIPT_Y, &pr))
  {
    mSuperscriptOffset = nscoord(MAX(f, NSToIntRound(pr * f)));
#ifdef REALLY_NOISY_FONTS
    printf("superscriptOffset=%d\n", mSuperscriptOffset);
#endif
  }
  else
  {
    mSuperscriptOffset = mXHeight;
  }

  if (::XGetFontProperty(mFontHandle, XA_SUBSCRIPT_Y, &pr))
  {
    mSubscriptOffset = nscoord(MAX(f, NSToIntRound(pr * f)));
#ifdef REALLY_NOISY_FONTS
    printf("subscriptOffset=%d\n", mSubscriptOffset);
#endif
  }
  else
  {
    mSubscriptOffset = mXHeight;
  }

  /* need better way to calculate this */
  mStrikeoutOffset = NSToIntRound((mAscent + 1) / 2);
  mStrikeoutSize = mUnderlineSize;

  mLeading = 0;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetXHeight(nscoord& aResult)
{
  aResult = mXHeight;
  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetSuperscriptOffset(nscoord& aResult)
{
  aResult = mSuperscriptOffset;
  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetSubscriptOffset(nscoord& aResult)
{
  aResult = mSubscriptOffset;
  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetStrikeout(nscoord& aOffset, nscoord& aSize)
{
  aOffset = mStrikeoutOffset;
  aSize = mStrikeoutSize;
  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetUnderline(nscoord& aOffset, nscoord& aSize)
{
  aOffset = mUnderlineOffset;
  aSize = mUnderlineSize;
  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetHeight(nscoord &aHeight)
{
  aHeight = mHeight;
  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetLeading(nscoord &aLeading)
{
  aLeading = mLeading;
  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetMaxAscent(nscoord &aAscent)
{
  aAscent = mMaxAscent;
  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetMaxDescent(nscoord &aDescent)
{
  aDescent = mMaxDescent;
  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetMaxAdvance(nscoord &aAdvance)
{
  aAdvance = mMaxAdvance;
  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetFont(const nsFont*& aFont)
{
  aFont = mFont;
  return NS_OK;
}

NS_IMETHODIMP  nsFontMetricsXlib::GetFontHandle(nsFontHandle &aHandle)
{
  aHandle = (nsFontHandle)mFontHandle;
  return NS_OK;
}

// ===================== new code -- erik ====================

#ifdef FONT_SWITCHING

/*
 * CSS2 "font properties":
 *   font-family
 *   font-style
 *   font-variant
 *   font-weight
 *   font-stretch
 *   font-size
 *   font-size-adjust
 *   font
 */

/*
 * CSS2 "font descriptors":
 *   font-family
 *   font-style
 *   font-variant
 *   font-weight
 *   font-stretch
 *   font-size
 *   unicode-range
 *   units-per-em
 *   src
 *   panose-1
 *   stemv
 *   stemh
 *   slope
 *   cap-height
 *   x-height
 *   ascent
 *   descent
 *   widths
 *   bbox
 *   definition-src
 *   baseline
 *   centerline
 *   mathline
 *   topline
 */

/*
 * XLFD 1.5 "FontName fields":
 *   FOUNDRY
 *   FAMILY_NAME
 *   WEIGHT_NAME
 *   SLANT
 *   SETWIDTH_NAME
 *   ADD_STYLE_NAME
 *   PIXEL_SIZE
 *   POINT_SIZE
 *   RESOLUTION_X
 *   RESOLUTION_Y
 *   SPACING
 *   AVERAGE_WIDTH
 *   CHARSET_REGISTRY
 *   CHARSET_ENCODING
 * XLFD example:
 *   -adobe-times-medium-r-normal--17-120-100-100-p-84-iso8859-1
 */

/*
 * XLFD 1.5 "font properties":
 *   FOUNDRY
 *   FAMILY_NAME
 *   WEIGHT_NAME
 *   SLANT
 *   SETWIDTH_NAME
 *   ADD_STYLE_NAME
 *   PIXEL_SIZE
 *   POINT_SIZE
 *   RESOLUTION_X
 *   RESOLUTION_Y
 *   SPACING
 *   AVERAGE_WIDTH
 *   CHARSET_REGISTRY
 *   CHARSET_ENCODING
 *   MIN_SPACE
 *   NORM_SPACE
 *   MAX_SPACE
 *   END_SPACE
 *   AVG_CAPITAL_WIDTH
 *   AVG_LOWERCASE_WIDTH
 *   QUAD_WIDTH
 *   FIGURE_WIDTH
 *   SUPERSCRIPT_X
 *   SUPERSCRIPT_Y
 *   SUBSCRIPT_X
 *   SUBSCRIPT_Y
 *   SUPERSCRIPT_SIZE
 *   SUBSCRIPT_SIZE
 *   SMALL_CAP_SIZE
 *   UNDERLINE_POSITION
 *   UNDERLINE_THICKNESS
 *   STRIKEOUT_ASCENT
 *   STRIKEOUT_DESCENT
 *   ITALIC_ANGLE
 *   CAP_HEIGHT
 *   X_HEIGHT
 *   RELATIVE_SETWIDTH
 *   RELATIVE_WEIGHT
 *   WEIGHT
 *   RESOLUTION
 *   FONT
 *   FACE_NAME
 *   FULL_NAME
 *   COPYRIGHT
 *   NOTICE
 *   DESTINATION
 *   FONT_TYPE
 *   FONT_VERSION
 *   RASTERIZER_NAME
 *   RASTERIZER_VERSION
 *   RAW_ASCENT
 *   RAW_DESCENT
 *   RAW_*
 *   AXIS_NAMES
 *   AXIS_LIMITS
 *   AXIS_TYPES
 */

/*
 * XLFD 1.5 BDF 2.1 properties:
 *   FONT_ASCENT
 *   FONT_DESCENT
 *   DEFAULT_CHAR
 */

/*
 * CSS2 algorithm, in the following order:
 *   font-family:  FAMILY_NAME (and FOUNDRY? (XXX))
 *   font-style:   SLANT (XXX: XLFD's RI and RO)
 *   font-variant: implemented in mozilla/layout/html/base/src/nsTextFrame.cpp
 *   font-weight:  RELATIVE_WEIGHT (XXX), WEIGHT (XXX), WEIGHT_NAME
 *   font-size:    XFontStruct.max_bounds.ascent + descent
 *
 * The following property is not specified in the algorithm spec. It will be
 * inserted between the font-weight and font-size steps for now:
 *   font-stretch: RELATIVE_SETWIDTH (XXX), SETWIDTH_NAME
 */

/*
 * XXX: Things to investigate in the future:
 *   ADD_STYLE_NAME font-family's serif and sans-serif
 *   SPACING        font-family's monospace; however, there are very few
 *                  proportional fonts in non-Latin-1 charsets, so beware in
 *                  font prefs dialog
 *   AVERAGE_WIDTH  none (see SETWIDTH_NAME)
 */

struct nsFontCharSetInfo
{
  const char*            mCharSet;
  nsFontCharSetConverter Convert;
  PRUint8                mSpecialUnderline;
  PRUint32*              mMap;
  nsIUnicodeEncoder*     mConverter;
};

struct nsFontStretch
{
  NS_DECL_AND_IMPL_ZEROING_OPERATOR_NEW

  void SortSizes(void);

  nsFontXlib*        mSizes;
  PRUint16           mSizesAlloc;
  PRUint16           mSizesCount;

  char*              mScalable;
  nsFontXlib**       mScaledFonts;
  PRUint16           mScaledFontsAlloc;
  PRUint16           mScaledFontsCount;
};

struct nsFontWeight
{
  NS_DECL_AND_IMPL_ZEROING_OPERATOR_NEW

  void FillStretchHoles(void);

  nsFontStretch* mStretches[9];
};

struct nsFontStyle
{
  NS_DECL_AND_IMPL_ZEROING_OPERATOR_NEW

  void FillWeightHoles(void);

  nsFontWeight* mWeights[9];
};

struct nsFontCharSet
{
  NS_DECL_AND_IMPL_ZEROING_OPERATOR_NEW

  void FillStyleHoles(void);

  nsFontCharSetInfo* mInfo;
  nsFontStyle*       mStyles[3];
  PRUint8            mHolesFilled;
};

struct nsFontFamily
{
  NS_DECL_AND_IMPL_ZEROING_OPERATOR_NEW

  PLHashTable* mCharSets;
};

typedef struct nsFontFamilyName
{
  char* mName;
  char* mXName;
} nsFontFamilyName;

typedef struct nsFontPropertyName
{
  char* mName;
  int   mValue;
} nsFontPropertyName;

typedef struct nsFontCharSetMap
{
  char*              mName;
  nsFontCharSetInfo* mInfo;
} nsFontCharSetMap;

static PLHashTable* gFamilies = nsnull;

static PLHashTable* gFamilyNames = nsnull;

static nsFontFamilyName gFamilyNameTable[] =
{
  { "arial",           "helvetica" },
  { "courier new",     "courier" },
  { "times new roman", "times" },

  { "serif",           "times" },
  { "sans-serif",      "helvetica" },
  { "fantasy",         "courier" },
  { "cursive",         "courier" },
  { "monospace",       "courier" },

  { nsnull, nsnull }
};

static PLHashTable* gWeights = nsnull;

static nsFontPropertyName gWeightNames[] =
{
  { "black",    900 },
  { "bold",     700 },
  { "book",     400 },
  { "demi",     600 },
  { "demibold", 600 },
  { "light",    300 },
  { "medium",   400 },
  { "regular",  400 },
  
  { nsnull,     0 }
};

static PLHashTable* gStretches = nsnull;

static nsFontPropertyName gStretchNames[] =
{
  { "block",         5 }, // XXX
  { "bold",          7 }, // XXX
  { "double wide",   9 },
  { "medium",        5 },
  { "narrow",        3 },
  { "normal",        5 },
  { "semicondensed", 4 },
  { "wide",          7 },

  { nsnull,          0 }
};

static PLHashTable* gCharSets = nsnull;

static nsFontCharSetInfo Ignore = { nsnull };

static void
SetUpFontCharSetInfo(nsFontCharSetInfo* aSelf)
{
  nsresult result;
  NS_WITH_SERVICE(nsICharsetConverterManager, manager,
                  NS_CHARSETCONVERTERMANAGER_PROGID, &result);
  if (manager && NS_SUCCEEDED(result)) {
    nsAutoString charset(aSelf->mCharSet);
    nsIUnicodeEncoder* converter = nsnull;
    result = manager->GetUnicodeEncoder(&charset, &converter);
    if (converter && NS_SUCCEEDED(result)) {
      aSelf->mConverter = converter;
      result = converter->SetOutputErrorBehavior(converter->kOnError_Replace,
                                                 nsnull, '?');
      nsCOMPtr<nsICharRepresentable> mapper = do_QueryInterface(converter);
      if (mapper) {
        result = mapper->FillInfo(aSelf->mMap);
      }
    }
  }
}

static int
SingleByteConvert(nsFontCharSetInfo* aSelf, const PRUnichar* aSrcBuf,
                  PRInt32 aSrcLen, char* aDestBuf, PRInt32 aDestLen)
{
  int count = 0;
  if (aSelf->mConverter) {
    aSelf->mConverter->Convert(aSrcBuf, &aSrcLen, aDestBuf, &aDestLen);
    count = aDestLen;
  }
  return count;
}

static int
DoubleByteConvert(nsFontCharSetInfo* aSelf, const PRUnichar* aSrcBuf,
                  PRInt32 aSrcLen, char* aDestBuf, PRInt32 aDestLen)
{
  int count = 0;
  if (aSelf->mConverter) {
    aSelf->mConverter->Convert(aSrcBuf, &aSrcLen, aDestBuf, &aDestLen);
    count = aDestLen;
  }
  // XXX do high-bit if font requires it
  return count;
}
static nsFontCharSetInfo CP1251 =
{ "windows-1251", SingleByteConvert, 0 };
static nsFontCharSetInfo ISO88591 =
{ "iso-8859-1", SingleByteConvert, 0 };
static nsFontCharSetInfo ISO88592 =
{ "iso-8859-2", SingleByteConvert, 0 };
static nsFontCharSetInfo ISO88593 =
{ "iso-8859-3", SingleByteConvert, 0 };
static nsFontCharSetInfo ISO88594 =
{ "iso-8859-4", SingleByteConvert, 0 };
static nsFontCharSetInfo ISO88595 =
{ "iso-8859-5", SingleByteConvert, 0 };
static nsFontCharSetInfo ISO88596 =
{ "iso-8859-6", SingleByteConvert, 0 };
static nsFontCharSetInfo ISO88597 =
{ "iso-8859-7", SingleByteConvert, 0 };
static nsFontCharSetInfo ISO88598 =
{ "iso-8859-8", SingleByteConvert, 0 };
static nsFontCharSetInfo ISO88599 =
{ "iso-8859-9", SingleByteConvert, 0 };
static nsFontCharSetInfo ISO885915 =
{ "iso-8859-15", SingleByteConvert, 0 };
static nsFontCharSetInfo JISX0201 =
{ "jis_0201", SingleByteConvert, 1 };
static nsFontCharSetInfo KOI8R =
{ "KOI8-R", SingleByteConvert, 0 };
static nsFontCharSetInfo Big5 =
{ "x-x-big5", DoubleByteConvert, 1 };
static nsFontCharSetInfo CNS116431 =
{ "x-cns-11643-1", DoubleByteConvert, 1 };
static nsFontCharSetInfo CNS116432 =
{ "x-cns-11643-2", DoubleByteConvert, 1 };
static nsFontCharSetInfo GB2312 =
{ "gb_2312-80", DoubleByteConvert, 1 };
static nsFontCharSetInfo JISX0208 =
{ "jis_0208-1983", DoubleByteConvert, 1 };
static nsFontCharSetInfo JISX0212 =
{ "jis_0212-1990", DoubleByteConvert, 1 };
static nsFontCharSetInfo KSC5601 =
{ "ks_c_5601-1987", DoubleByteConvert, 1 };


/*
 * Normally, the charset of an X font can be determined simply by looking at
 * the last 2 fields of the long XLFD font name (CHARSET_REGISTRY and
 * CHARSET_ENCODING). However, there are a number of special cases:
 *
 * Sometimes, X server vendors use the same name to mean different things. For
 * example, IRIX uses "cns11643-1" to mean the 2nd plane of CNS 11643, while
 * Solaris uses that name for the 1st plane.
 *
 * Some X server vendors use certain names for something completely different.
 * For example, some Solaris fonts say "gb2312.1980-0" but are actually ASCII
 * fonts. These cases can be detected by looking at the POINT_SIZE and
 * AVERAGE_WIDTH fields. If the average width is half the point size, this is
 * an ASCII font, not GB 2312.
 *
 * Some fonts say "fontspecific" in the CHARSET_ENCODING field. Their charsets
 * depend on the FAMILY_NAME. For example, the following is a "Symbol" font:
 *
 *   -adobe-symbol-medium-r-normal--17-120-100-100-p-95-adobe-fontspecific
 *
 * Some vendors use one name to mean 2 different things, depending on the font.
 * For example, AIX has some "ksc5601.1987-0" fonts that require the 8th bit of
 * both bytes to be zero, while other fonts require them to be set to one.
 * These cases can be distinguished by looking at the FOUNDRY field, but a
 * better way is to look at XFontStruct.min_byte1.
 */
static nsFontCharSetMap gCharSetMap[] =
{
  { "-ascii",             &Ignore        },
  { "-ibm pc",            &Ignore        },
  { "adobe-fontspecific", &Ignore        },
  { "cns11643.1986-1",    &CNS116431     },
  { "cns11643.1986-2",    &CNS116432     },
  { "cns11643.1992-1",    &CNS116431     },
  { "cns11643.1992-12",   &Ignore        },
  { "cns11643.1992-2",    &CNS116432     },
  { "cns11643.1992-3",    &Ignore        },
  { "cns11643.1992-4",    &Ignore        },
  { "cp1251-1",           &CP1251        },
  { "dec-dectech",        &Ignore        },
  { "dtsymbol-1",         &Ignore        },
  { "fontspecific-0",     &Ignore        },
  { "gb2312.1980-0",      &GB2312        },
  { "gb2312.1980-1",      &GB2312        },
  { "hp-japanese15",      &Ignore        },
  { "hp-japaneseeuc",     &Ignore        },
  { "hp-roman8",          &Ignore        },
  { "hp-schinese15",      &Ignore        },
  { "hp-tchinese15",      &Ignore        },
  { "hp-tchinesebig5",    &Big5          },
  { "hp-wa",              &Ignore        },
  { "hpbig5-",            &Big5          },
  { "hproc16-",           &Ignore        },
  { "ibm-1252",           &Ignore        },
  { "ibm-850",            &Ignore        },
  { "ibm-fontspecific",   &Ignore        },
  { "ibm-sbdcn",          &Ignore        },
  { "ibm-sbdtw",          &Ignore        },
  { "ibm-special",        &Ignore        },
  { "ibm-udccn",          &Ignore        },
  { "ibm-udcjp",          &Ignore        },
  { "ibm-udctw",          &Ignore        },
  { "iso646.1991-irv",    &Ignore        },
  { "iso8859-1",          &ISO88591      },
  { "iso8859-15",         &ISO885915     },
  { "iso8859-1@cn",       &Ignore        },
  { "iso8859-1@kr",       &Ignore        },
  { "iso8859-1@tw",       &Ignore        },
  { "iso8859-1@zh",       &Ignore        },
  { "iso8859-2",          &ISO88592      },
  { "iso8859-3",          &ISO88593      },
  { "iso8859-4",          &ISO88594      },
  { "iso8859-5",          &ISO88595      },
  { "iso8859-6",          &ISO88596      },
  { "iso8859-7",          &ISO88597      },
  { "iso8859-8",          &ISO88598      },
  { "iso8859-9",          &ISO88599      },
  { "iso10646-1",         &Ignore        },
  { "jisx0201.1976-0",    &JISX0201      },
  { "jisx0201.1976-1",    &JISX0201      },
  { "jisx0208.1983-0",    &JISX0208      },
  { "jisx0208.1990-0",    &JISX0208      },
  { "jisx0212.1990-0",    &JISX0212      },
  { "koi8-r",             &KOI8R         },
  { "ksc5601.1987-0",     &KSC5601       },
  { "misc-fontspecific",  &Ignore        },
  { "sgi-fontspecific",   &Ignore        },
  { "sun-fontspecific",   &Ignore        },
  { "sunolcursor-1",      &Ignore        },
  { "sunolglyph-1",       &Ignore        },
  { "ucs2.cjk-0",         &Ignore        },
  { "ucs2.cjk_japan-0",   &Ignore        },
  { "ucs2.cjk_taiwan-0",  &Ignore        },

  { nsnull,               nsnull         }
};

#undef DEBUG_DUMP_TREE
#ifdef DEBUG_DUMP_TREE

static char* gDumpStyles[3] = { "normal", "italic", "oblique" };

static PRIntn
DumpCharSet(PLHashEntry* he, PRIntn i, void* arg)
{
  printf("        %s\n", (char*) he->key);
  nsFontCharSet* charSet = (nsFontCharSet*) he->value;
  for (int sizeIndex = 0; sizeIndex < charSet->mSizesCount; sizeIndex++) {
    nsFontXlib* size = &charSet->mSizes[sizeIndex];
    printf("          %d %s\n", size->mSize, size->mName);
  }
  return HT_ENUMERATE_NEXT;
}

static void
DumpFamily(nsFontFamily* aFamily)
{
  for (int styleIndex = 0; styleIndex < 3; styleIndex++) {
    nsFontStyle* style = aFamily->mStyles[styleIndex];
    if (style) {
      printf("  style: %s\n", gDumpStyles[styleIndex]);
      for (int weightIndex = 0; weightIndex < 8; weightIndex++) {
        nsFontWeight* weight = style->mWeights[weightIndex];
        if (weight) {
          printf("    weight: %d\n", (weightIndex + 1) * 100);
          for (int stretchIndex = 0; stretchIndex < 9; stretchIndex++) {
            nsFontStretch* stretch = weight->mStretches[stretchIndex];
            if (stretch) {
              printf("      stretch: %d\n", stretchIndex + 1);
              PL_HashTableEnumerateEntries(stretch->mCharSets, DumpCharSet,
                nsnull);
            }
          }
        }
      }
    }
  }
}

static PRIntn
DumpFamilyEnum(PLHashEntry* he, PRIntn i, void* arg)
{
  char buf[256];
  ((nsString*) he->key)->ToCString(buf, sizeof(buf));
  printf("family: %s\n", buf);
  nsFontFamily* family = (nsFontFamily*) he->value;
  DumpFamily(family);

  return HT_ENUMERATE_NEXT;
}

static void
DumpTree(void)
{
  PL_HashTableEnumerateEntries(gFamilies, DumpFamilyEnum, nsnull);
}

#endif /* DEBUG_DUMP_TREE */

static PLHashNumber
HashKey(const void* aString)
{
  return (PLHashNumber)
    nsCRT::HashValue(((const nsString*) aString)->GetUnicode());
}

static PRIntn
CompareKeys(const void* aStr1, const void* aStr2)
{
  return nsCRT::strcmp(((const nsString*) aStr1)->GetUnicode(),
    ((const nsString*) aStr2)->GetUnicode()) == 0;
}

struct nsFontSearch
{
  nsFontMetricsXlib* mMetrics;
  PRUnichar          mChar;
  nsFontXlib*        mFont;
};

static void
GetUnderlineInfo(XFontStruct* aFont, unsigned long* aPositionX2,
  unsigned long* aThickness)
{
  /*
   * XLFD 1.5 says underline position defaults descent/2.
   * Hence we return position*2 to avoid rounding error.
   */
  if (::XGetFontProperty(aFont, XA_UNDERLINE_POSITION, aPositionX2)) {
    *aPositionX2 *= 2;
  }
  else {
    *aPositionX2 = aFont->max_bounds.descent;
  }

  /*
   * XLFD 1.5 says underline thickness defaults to cap stem width.
   * We don't know what that is, so we just take the thickness of "_".
   * This way, we get thicker underlines for bold fonts.
   */
  if (!::XGetFontProperty(aFont, XA_UNDERLINE_THICKNESS, aThickness)) {
    int dir, ascent, descent;
    XCharStruct overall;
    XTextExtents(aFont, "_", 1, &dir, &ascent, &descent, &overall);
    *aThickness = (overall.ascent + overall.descent);
  }
}

void
nsFontXlib::LoadFont(nsFontCharSet* aCharSet, nsFontMetricsXlib* aMetrics)
{
  XFontStruct *xlibFont = XLoadQueryFont(gDisplay, mName);
  if (xlibFont) {
    mFont = xlibFont;
    mMap = aCharSet->mInfo->mMap;
    mActualSize = xlibFont->max_bounds.ascent + xlibFont->max_bounds.descent;
    if (aCharSet->mInfo->mSpecialUnderline) {
      XFontStruct* asciiXFont = aMetrics->mFontHandle;
      unsigned long positionX2;
      unsigned long thickness;
      GetUnderlineInfo(asciiXFont, &positionX2, &thickness);
      mActualSize += (positionX2 + thickness);
      mBaselineAdjust = (-xlibFont->max_bounds.descent);
    }
  }
}

void
PickASizeAndLoad(nsFontSearch* aSearch, nsFontStretch* aStretch,
  nsFontCharSet* aCharSet)
{
  nsFontXlib* s = nsnull;
  nsFontXlib* begin;
  nsFontXlib* end;
  nsFontMetricsXlib* m = aSearch->mMetrics;
  int desiredSize = m->mPixelSize;
  int scalable = 0;

  if (aStretch->mSizes) {
    begin = aStretch->mSizes;
    end = &aStretch->mSizes[aStretch->mSizesCount];
    for (s = begin; s < end; s++) {
      if (s->mSize >= desiredSize) {
        break;
      }
    }
    if (s == end) {
      s--;
    }
    else if (s != begin) {
      if ((s->mSize - desiredSize) > (desiredSize - (s - 1)->mSize)) {
        s--;
      }
    }
  
    if (!s->mFont) {
      s->LoadFont(aCharSet, m);
      if (!s->mFont) {
        return;
      }
    }
    if (s->mActualSize > desiredSize) {
      for (; s >= begin; s--) {
        if (!s->mFont) {
          s->LoadFont(aCharSet, m);
          if (!s->mFont) {
            return;
          }
        }
        if (s->mActualSize <= desiredSize) {
          if (((s + 1)->mActualSize - desiredSize) <=
              (desiredSize - s->mActualSize)) {
            s++;
          }
          break;
        }
      }
      if (s < begin) {
        s = begin;
      }
    }
    else if (s->mActualSize < desiredSize) {
      for (; s < end; s++) {
        if (!s->mFont) {
          s->LoadFont(aCharSet, m);
          if (!s->mFont) {
            return;
          }
        }
        if (s->mActualSize >= desiredSize) {
          if ((s->mActualSize - desiredSize) >
              (desiredSize - (s - 1)->mActualSize)) {
            s--;
          }
          break;
        }
      }
      if (s == end) {
        s--;
      }
    }

    if (aStretch->mScalable) {
      double ratio = (s->mActualSize / ((double) desiredSize));
      if ((ratio > 1.2) || (ratio < 0.8)) {
        scalable = 1;
      }
    }
  }
  else {
    scalable = 1;
  }

  if (scalable) {
    nsFontXlib* closestBitmapSize = s;
    nsFontXlib** beginScaled = aStretch->mScaledFonts;
    nsFontXlib** endScaled =
      &aStretch->mScaledFonts[aStretch->mScaledFontsCount];
    nsFontXlib** p;
    for (p = beginScaled; p < endScaled; p++) {
      if ((*p)->mSize == desiredSize) {
        break;
      }
    }
    if (p == endScaled) {
      s = new nsFontXlib;
      if (s) {
        s->mName = PR_smprintf(aStretch->mScalable, desiredSize);
        if (!s->mName) {
          delete s;
          return;
        }
        s->mSize = desiredSize;
        s->mCharSetInfo = aCharSet->mInfo;
        s->LoadFont(aCharSet, m);
        if (s->mFont) {
          if (aStretch->mScaledFontsCount == aStretch->mScaledFontsAlloc) {
            int newSize = 2 *
              (aStretch->mScaledFontsAlloc ? aStretch->mScaledFontsAlloc : 1);
            nsFontXlib** newPointer = (nsFontXlib**)
              PR_Realloc(aStretch->mScaledFonts, newSize * sizeof(nsFontXlib*));
            if (newPointer) {
              aStretch->mScaledFontsAlloc = newSize;
              aStretch->mScaledFonts = newPointer;
            }
            else {
              delete s;
              return;
            }
          }
          aStretch->mScaledFonts[aStretch->mScaledFontsCount++] = s;
        }
        else {
          delete s;
          s = nsnull;
        }
      }
      if (!s) {
        if (closestBitmapSize) {
          s = closestBitmapSize;
        }
        else {
          return;
        }
      }
    }
    else {
      s = *p;
    }
  }

  if (m->mLoadedFontsCount == m->mLoadedFontsAlloc) {
    int newSize;
    if (m->mLoadedFontsAlloc) {
      newSize = (2 * m->mLoadedFontsAlloc);
    }
    else {
      newSize = 1;
    }
    nsFontXlib** newPointer = (nsFontXlib**) PR_Realloc(m->mLoadedFonts,
      newSize * sizeof(nsFontXlib*));
    if (newPointer) {
      m->mLoadedFonts = newPointer;
      m->mLoadedFontsAlloc = newSize;
    }
    else {
      return;
    }
  }
  m->mLoadedFonts[m->mLoadedFontsCount++] = s;
  aSearch->mFont = s;

#if 0
  nsFontXlib* result = s;
  for (s = begin; s < end; s++) {
    printf("%d/%d ", s->mSize, s->mActualSize);
  }
  printf("desired %d chose %d\n", desiredSize, result->mActualSize);
#endif /* 0 */
}

static int
CompareSizes(const void* aArg1, const void* aArg2, void *data)
{
  return ((nsFontXlib*) aArg1)->mSize - ((nsFontXlib*) aArg2)->mSize;
}

void
nsFontStretch::SortSizes(void)
{
  NS_QuickSort(mSizes, mSizesCount, sizeof(*mSizes), CompareSizes, NULL);
}

void
nsFontWeight::FillStretchHoles(void)
{
  int i, j;

  for (i = 0; i < 9; i++) {
    if (mStretches[i]) {
      mStretches[i]->SortSizes();
    }
  }

  if (!mStretches[4]) {
    for (i = 5; i < 9; i++) {
      if (mStretches[i]) {
        mStretches[4] = mStretches[i];
        break;
      }
    }
    if (!mStretches[4]) {
      for (i = 3; i >= 0; i--) {
        if (mStretches[i]) {
          mStretches[4] = mStretches[i];
          break;
        }
      }
    }
  }

  for (i = 5; i < 9; i++) {
    if (!mStretches[i]) {
      for (j = i + 1; j < 9; j++) {
        if (mStretches[j]) {
          mStretches[i] = mStretches[j];
          break;
        }
      }
      if (!mStretches[i]) {
        for (j = i - 1; j >= 0; j--) {
          if (mStretches[j]) {
            mStretches[i] = mStretches[j];
            break;
          }
        }
      }
    }
  }
  for (i = 3; i >= 0; i--) {
    if (!mStretches[i]) {
      for (j = i - 1; j >= 0; j--) {
        if (mStretches[j]) {
          mStretches[i] = mStretches[j];
          break;
        }
      }
      if (!mStretches[i]) {
        for (j = i + 1; j < 9; j++) {
          if (mStretches[j]) {
            mStretches[i] = mStretches[j];
            break;
          }
        }
      }
    }
  }
}

void
nsFontStyle::FillWeightHoles(void)
{
  int i, j;

  for (i = 0; i < 9; i++) {
    if (mWeights[i]) {
      mWeights[i]->FillStretchHoles();
    }
  }

  if (!mWeights[3]) {
    for (i = 4; i < 9; i++) {
      if (mWeights[i]) {
        mWeights[3] = mWeights[i];
        break;
      }
    }
    if (!mWeights[3]) {
      for (i = 2; i >= 0; i--) {
        if (mWeights[i]) {
          mWeights[3] = mWeights[i];
          break;
        }
      }
    }
  }

  // CSS2, section 15.5.1
  if (!mWeights[4]) {
    mWeights[4] = mWeights[3];
  }
  for (i = 5; i < 9; i++) {
    if (!mWeights[i]) {
      for (j = i + 1; j < 9; j++) {
        if (mWeights[j]) {
          mWeights[i] = mWeights[j];
          break;
        }
      }
      if (!mWeights[i]) {
        for (j = i - 1; j >= 0; j--) {
          if (mWeights[j]) {
            mWeights[i] = mWeights[j];
            break;
          }
        }
      }
    }
  }
  for (i = 2; i >= 0; i--) {
    if (!mWeights[i]) {
      for (j = i - 1; j >= 0; j--) {
        if (mWeights[j]) {
          mWeights[i] = mWeights[j];
          break;
        }
      }
      if (!mWeights[i]) {
        for (j = i + 1; j < 9; j++) {
          if (mWeights[j]) {
            mWeights[i] = mWeights[j];
            break;
          }
        }
      }
    }
  }
}

void
nsFontCharSet::FillStyleHoles(void)
{
  if (mHolesFilled) {
    return;
  }
  mHolesFilled = 1;

#ifdef DEBUG_DUMP_TREE
  DumpFamily(this);
#endif

  for (int i = 0; i < 3; i++) {
    if (mStyles[i]) {
      mStyles[i]->FillWeightHoles();
    }
  }

  // XXX If both italic and oblique exist, there is probably something
  // wrong. Try counting the fonts, and removing the one that has less.
  if (!mStyles[NS_FONT_STYLE_NORMAL]) {
    if (mStyles[NS_FONT_STYLE_ITALIC]) {
      mStyles[NS_FONT_STYLE_NORMAL] = mStyles[NS_FONT_STYLE_ITALIC];
    }
    else {
      mStyles[NS_FONT_STYLE_NORMAL] = mStyles[NS_FONT_STYLE_OBLIQUE];
    }
  }
  if (!mStyles[NS_FONT_STYLE_ITALIC]) {
    if (mStyles[NS_FONT_STYLE_OBLIQUE]) {
      mStyles[NS_FONT_STYLE_ITALIC] = mStyles[NS_FONT_STYLE_OBLIQUE];
    }
    else {
      mStyles[NS_FONT_STYLE_ITALIC] = mStyles[NS_FONT_STYLE_NORMAL];
    }
  }
  if (!mStyles[NS_FONT_STYLE_OBLIQUE]) {
    if (mStyles[NS_FONT_STYLE_ITALIC]) {
      mStyles[NS_FONT_STYLE_OBLIQUE] = mStyles[NS_FONT_STYLE_ITALIC];
    }
    else {
      mStyles[NS_FONT_STYLE_OBLIQUE] = mStyles[NS_FONT_STYLE_NORMAL];
    }
  }

#ifdef DEBUG_DUMP_TREE
  DumpFamily(this);
#endif
}

#define WEIGHT_INDEX(weight) (((weight) / 100) - 1)

#define GET_WEIGHT_INDEX(index, weight) \
  do {                                  \
    (index) = WEIGHT_INDEX(weight);     \
    if ((index) < 0) {                  \
      (index) = 0;                      \
    }                                   \
    else if ((index) > 8) {             \
      (index) = 8;                      \
    }                                   \
  } while (0)

void
TryCharSet(nsFontSearch* aSearch, nsFontCharSet* aCharSet)
{
  aCharSet->FillStyleHoles();
  nsFontMetricsXlib* f = aSearch->mMetrics;
  nsFontStyle* style = aCharSet->mStyles[f->mStyleIndex];
  if (!style) {
    return; // skip dummy entries
  }

  nsFontWeight** weights = style->mWeights;
  int weight = f->mFont->weight;
  int steps = (weight % 100);
  int weightIndex;
  if (steps) {
    if (steps < 10) {
      int base = (weight - (steps * 101));
      GET_WEIGHT_INDEX(weightIndex, base);
      while (steps--) {
        nsFontWeight* prev = weights[weightIndex];
        for (weightIndex++; weightIndex < 9; weightIndex++) {
          if (weights[weightIndex] != prev) {
            break;
          }
        }
        if (weightIndex >= 9) {
          weightIndex = 8;
        }
      }
    }
    else if (steps > 90) {
      steps = (100 - steps);
      int base = (weight + (steps * 101));
      GET_WEIGHT_INDEX(weightIndex, base);
      while (steps--) {
        nsFontWeight* prev = weights[weightIndex];
        for (weightIndex--; weightIndex >= 0; weightIndex--) {
          if (weights[weightIndex] != prev) {
            break;
          }
        }
        if (weightIndex < 0) {
          weightIndex = 0;
        }
      }
    }
    else {
      GET_WEIGHT_INDEX(weightIndex, weight);
    }
  }
  else {
    GET_WEIGHT_INDEX(weightIndex, weight);
  }

  PickASizeAndLoad(aSearch, weights[weightIndex]->mStretches[f->mStretchIndex],
    aCharSet);
}

static PRIntn
SearchCharSet(PLHashEntry* he, PRIntn i, void* arg)
{
  nsFontCharSet* charSet = (nsFontCharSet*) he->value;
  nsFontCharSetInfo* charSetInfo = charSet->mInfo;
  PRUint32* map = charSetInfo->mMap;
  nsFontSearch* search = (nsFontSearch*) arg;
  PRUnichar c = search->mChar;
  if (!map) {
    map = (PRUint32*) PR_Calloc(2048, 4);
    if (!map) {
      return HT_ENUMERATE_NEXT;
    }
    charSetInfo->mMap = map;
    SetUpFontCharSetInfo(charSetInfo);
  }
  if (!IS_REPRESENTABLE(map, c)) {
    return HT_ENUMERATE_NEXT;
  }

  TryCharSet(search, charSet);
  if (search->mFont) {
    return HT_ENUMERATE_STOP;
  }

  return HT_ENUMERATE_NEXT;
}

void
TryFamily(nsFontSearch* aSearch, nsFontFamily* aFamily)
{
  // XXX Should process charsets in reasonable order, instead of randomly
  // enumerating the hash table.
  if (aFamily->mCharSets) {
    PL_HashTableEnumerateEntries(aFamily->mCharSets, SearchCharSet, aSearch);
  }
}

static PRIntn
SearchFamily(PLHashEntry* he, PRIntn i, void* arg)
{
  nsFontFamily* family = (nsFontFamily*) he->value;
  nsFontSearch* search = (nsFontSearch*) arg;
  TryFamily(search, family);
  if (search->mFont) {
    return HT_ENUMERATE_STOP;
  }

  return HT_ENUMERATE_NEXT;
}

static nsFontFamily*
GetFontNames(char* aPattern)
{
  nsFontFamily* family = nsnull;

  int count;
  //printf("XListFonts %s\n", aPattern);
  char** list = ::XListFonts(gDisplay, aPattern, INT_MAX, &count);
  if ((!list) || (count < 1)) {
    return nsnull;
  }
  for (int i = 0; i < count; i++) {
    char* name = list[i];
    if ((!name) || (name[0] != '-')) {
      continue;
    }
    char* p = name + 1;
    int scalable = 0;

#ifdef FIND_FIELD
#undef FIND_FIELD
#endif
#define FIND_FIELD(var)           \
  char* var = p;                  \
  while ((*p) && ((*p) != '-')) { \
    p++;                          \
  }                               \
  if (*p) {                       \
    *p++ = 0;                     \
  }                               \
  else {                          \
    continue;                     \
  }

#ifdef SKIP_FIELD
#undef SKIP_FIELD
#endif
#define SKIP_FIELD(var)           \
  while ((*p) && ((*p) != '-')) { \
    p++;                          \
  }                               \
  if (*p) {                       \
    p++;                          \
  }                               \
  else {                          \
    continue;                     \
  }

    SKIP_FIELD(foundry);
    // XXX What to do about the many Applix fonts that start with "ax"?
    FIND_FIELD(familyName);
    FIND_FIELD(weightName);
    FIND_FIELD(slant);
    FIND_FIELD(setWidth);
    FIND_FIELD(addStyle);
    FIND_FIELD(pixelSize);
    if (pixelSize[0] == '0') {
      scalable = 1;
    }
    SKIP_FIELD(pointSize);
    SKIP_FIELD(resolutionX);
    SKIP_FIELD(resolutionY);
    FIND_FIELD(spacing);
    SKIP_FIELD(averageWidth);
    char* charSetName = p; // CHARSET_REGISTRY & CHARSET_ENCODING
    if (!*charSetName) {
      continue;
    }
    nsFontCharSetInfo* charSetInfo =
      (nsFontCharSetInfo*) PL_HashTableLookup(gCharSets, charSetName);
    if (!charSetInfo) {
#ifdef NOISY_FONTS
      printf("cannot find charset %s\n", charSetName);
#endif
      continue;
    }
    if (charSetInfo == &Ignore) {
      // XXX printf("ignoring %s\n", charSetName);
      continue;
    }

    nsAutoString familyName2(familyName);
    family =
      (nsFontFamily*) PL_HashTableLookup(gFamilies, (nsString*) &familyName2);
    if (!family) {
      family = new nsFontFamily;
      if (!family) {
        continue;
      }
      nsString* copy = new nsString(familyName);
      if (!copy) {
        delete family;
        continue;
      }
      PL_HashTableAdd(gFamilies, copy, family);
    }

    if (!family->mCharSets) {
      family->mCharSets = PL_NewHashTable(0, PL_HashString, PL_CompareStrings,
        NULL, NULL, NULL);
      if (!family->mCharSets) {
        continue;
      }
    }
    nsFontCharSet* charSet =
      (nsFontCharSet*) PL_HashTableLookup(family->mCharSets, charSetName);
    if (!charSet) {
      charSet = new nsFontCharSet;
      if (!charSet) {
        continue;
      }
      char* copy = strdup(charSetName);
      if (!copy) {
        delete charSet;
        continue;
      }
      charSet->mInfo = charSetInfo;
      PL_HashTableAdd(family->mCharSets, copy, charSet);
    }

    int styleIndex;
    // XXX This does not cover the full XLFD spec for SLANT.
    switch (slant[0]) {
    case 'i':
      styleIndex = NS_FONT_STYLE_ITALIC;
      break;
    case 'o':
      styleIndex = NS_FONT_STYLE_OBLIQUE;
      break;
    case 'r':
    default:
      styleIndex = NS_FONT_STYLE_NORMAL;
      break;
    }
    nsFontStyle* style = charSet->mStyles[styleIndex];
    if (!style) {
      style = new nsFontStyle;
      if (!style) {
        continue;
      }
      charSet->mStyles[styleIndex] = style;
    }

    int weightNumber = (int) PL_HashTableLookup(gWeights, weightName);
    if (!weightNumber) {
#ifdef NOISY_FONTS
      printf("cannot find weight %s\n", weightName);
#endif
      weightNumber = NS_FONT_WEIGHT_NORMAL;
    }
    int weightIndex = WEIGHT_INDEX(weightNumber);
    nsFontWeight* weight = style->mWeights[weightIndex];
    if (!weight) {
      weight = new nsFontWeight;
      if (!weight) {
        continue;
      }
      style->mWeights[weightIndex] = weight;
    }
  
    int stretchIndex = (int) PL_HashTableLookup(gStretches, setWidth);
    if (!stretchIndex) {
#ifdef NOISY_FONTS
      printf("cannot find stretch %s\n", setWidth);
#endif
      stretchIndex = 5;
    }
    stretchIndex--;
    nsFontStretch* stretch = weight->mStretches[stretchIndex];
    if (!stretch) {
      stretch = new nsFontStretch;
      if (!stretch) {
        continue;
      }
      weight->mStretches[stretchIndex] = stretch;
    }
    if (scalable) {
      if (!stretch->mScalable) {
        stretch->mScalable = PR_smprintf("%s-%s-%s-%s-%s-%%d-*-*-*-%s-*-%s",
          name, weightName, slant, setWidth, addStyle, spacing, charSetName);
      }
      continue;
    }
  
    int pixels = atoi(pixelSize);
    if (stretch->mSizesCount) {
      nsFontXlib* end = &stretch->mSizes[stretch->mSizesCount];
      nsFontXlib* s;
      for (s = stretch->mSizes; s < end; s++) {
        if (s->mSize == pixels) {
          break;
        }
      }
      if (s != end) {
        continue;
      }
    }
    if (stretch->mSizesCount == stretch->mSizesAlloc) {
      int newSize = 2 * (stretch->mSizesAlloc ? stretch->mSizesAlloc : 1);
      nsFontXlib* newPointer = new nsFontXlib[newSize];
      if (newPointer) {
        for (int j = stretch->mSizesAlloc - 1; j >= 0; j--) {
          newPointer[j] = stretch->mSizes[j];
        }
        stretch->mSizesAlloc = newSize;
        delete [] stretch->mSizes;
        stretch->mSizes = newPointer;
      }
      else {
        continue;
      }
    }
    p = name;
    while (p < charSetName) {
      if (!*p) {
        *p = '-';
      }
      p++;
    }
    char* copy = strdup(name);
    if (!copy) {
      continue;
    }
    nsFontXlib* size = &stretch->mSizes[stretch->mSizesCount++];
    size->mName = copy;
    size->mFont = nsnull;
    size->mSize = pixels;
    size->mActualSize = 0;
    size->mBaselineAdjust = 0;
    size->mMap = nsnull;
    size->mCharSetInfo = charSetInfo;
  }
  XFreeFontNames(list);

#ifdef DEBUG_DUMP_TREE
  //DumpTree();
#endif

  return family;
}

/*
 * XListFonts(*) is expensive. Delay this till the last possible moment.
 * XListFontsWithInfo is expensive. Use XLoadQueryFont instead.
 */
nsFontXlib*
nsFontMetricsXlib::FindFont(PRUnichar aChar)
{
  static int gInitialized = 0;
  if (!gInitialized) {
    gInitialized = 1;
    gFamilies = PL_NewHashTable(0, HashKey, CompareKeys, NULL, NULL, NULL);
    gFamilyNames = PL_NewHashTable(0, HashKey, CompareKeys, NULL, NULL, NULL);
    nsFontFamilyName* f = gFamilyNameTable;
    while (f->mName) {
      nsString* name = new nsString(f->mName);
      nsString* xName = new nsString(f->mXName);
      if (name && xName) {
        PL_HashTableAdd(gFamilyNames, name, (void*) xName);
      }
      f++;
    }
    gWeights = PL_NewHashTable(0, PL_HashString, PL_CompareStrings, NULL, NULL,
      NULL);
    nsFontPropertyName* p = gWeightNames;
    while (p->mName) {
      PL_HashTableAdd(gWeights, p->mName, (void*) p->mValue);
      p++;
    }
    gStretches = PL_NewHashTable(0, PL_HashString, PL_CompareStrings, NULL,
      NULL, NULL);
    p = gStretchNames;
    while (p->mName) {
      PL_HashTableAdd(gStretches, p->mName, (void*) p->mValue);
      p++;
    }
    gCharSets = PL_NewHashTable(0, PL_HashString, PL_CompareStrings, NULL, NULL,
      NULL);
    nsFontCharSetMap* charSetMap = gCharSetMap;
    while (charSetMap->mName) {
      PL_HashTableAdd(gCharSets, charSetMap->mName, (void*) charSetMap->mInfo);
      charSetMap++;
    }
  }

  nsFontSearch search = { this, aChar, nsnull };

  while (mFontsIndex < mFontsCount) {
    nsString* familyName = &mFonts[mFontsIndex++];
    nsString* xName = (nsString*) PL_HashTableLookup(gFamilyNames, familyName);
    if (!xName) {
      xName = familyName;
    }
    nsFontFamily* family =
      (nsFontFamily*) PL_HashTableLookup(gFamilies, xName);
    if (!family) {
      char name[128];
      xName->ToCString(name, sizeof(name));
      char buf[256];
      PR_snprintf(buf, sizeof(buf), "-*-%s-*-*-*-*-*-*-*-*-*-*-*-*", name);
      family = GetFontNames(buf);
      if (!family) {
        family = new nsFontFamily; // dummy entry to avoid calling X again
        if (family) {
          nsString* copy = new nsString(*xName);
          if (copy) {
            PL_HashTableAdd(gFamilies, copy, family);
          }
          else {
            delete family;
          }
        }
        continue;
      }
    }
    TryFamily(&search, family);
    if (search.mFont) {
      return search.mFont;
    }
  }

  // XXX If we get to this point, that means that we have exhausted all the
  // families in the lists. Maybe we should try a list of fonts that are
  // specific to the vendor of the X server here. Because XListFonts for the
  // whole list is very expensive on some Unixes.

  static int gGotAllFontNames = 0;
  if (!gGotAllFontNames) {
    gGotAllFontNames = 1;
    GetFontNames("-*-*-*-*-*-*-*-*-*-*-*-*-*-*");
  }

  PL_HashTableEnumerateEntries(gFamilies, SearchFamily, &search);
  if (search.mFont) {
    // XXX We should probably write this family name out to disk, so that we
    // can use it next time. I.e. prefs file or something.
    return search.mFont;
  }

  // XXX Just return nsnull for now.
  // Need to draw boxes eventually. Or pop up dialog like plug-in dialog.
  return nsnull;
}

int
nsFontMetricsXlib::GetWidth(nsFontXlib* aFont, const PRUnichar* aString,
  PRUint32 aLength)
{
  XChar2b buf[512];
  int ret;
  int len = aFont->mCharSetInfo->Convert(aFont->mCharSetInfo, aString, aLength,
    (char*) buf, sizeof(buf));
  // XXX this is slow as dirt.
  XFontStruct *font_struct = aFont->mFont;
  ret = XTextWidth16(font_struct, buf, len / 2);
  return ret;
}

void
nsFontMetricsXlib::DrawString(nsDrawingSurfaceXlib* aSurface, nsFontXlib* aFont,
  nscoord aX, nscoord aY, const PRUnichar* aString, PRUint32 aLength)
{
  XChar2b buf[512];
  int len = aFont->mCharSetInfo->Convert(aFont->mCharSetInfo, aString, aLength,
    (char*) buf, sizeof(buf));
  XDrawString16(gDisplay,
                aSurface->GetDrawable(),
                aSurface->GetGC(),
                aX, aY, buf, (len / 2));
}

nsresult
nsFontMetricsXlib::GetSpaceWidth(nscoord &aSpaceWidth)
{
  aSpaceWidth = mSpaceWidth;
  return NS_OK;
}

#endif /* FONT_SWITCHING */
