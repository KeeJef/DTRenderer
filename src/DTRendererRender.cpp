#include "DTRendererRender.h"
#include "DTRendererPlatform.h"

#define STB_RECT_PACK_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#include "external/stb_rect_pack.h"
#include "external/stb_truetype.h"

// #define DTR_DEBUG_RENDER_FONT_BITMAP
#ifdef DTR_DEBUG_RENDER_FONT_BITMAP
	#define STB_IMAGE_WRITE_IMPLEMENTATION
	#include "external/stb_image_write.h"
#endif

inline DqnV4 DTRRender_PreMultiplyAlpha(const DqnV4 color)
{
	DqnV4 result;
	f32 normA = color.a / 255.0f;
	result.r  = color.r * normA;
	result.g  = color.g * normA;
	result.b  = color.b * normA;
	result.a  = color.a;

	return result;
}

// IMPORTANT(doyle): Color is expected to be premultiplied already
FILE_SCOPE inline void SetPixel(PlatformRenderBuffer *const renderBuffer,
                                const i32 x, const i32 y, const DqnV4 color)
{
	if (!renderBuffer) return;
	if (x <= 0 || x > (renderBuffer->width - 1)) return;
	if (y <= 0 || y > (renderBuffer->height - 1)) return;

	u32 *const bitmapPtr = (u32 *)renderBuffer->memory;
	const u32 pitchInU32 = (renderBuffer->width * renderBuffer->bytesPerPixel) / 4;

	f32 newA     = color.a;
	f32 newANorm = newA / 255.0f;
	f32 newR     = color.r;
	f32 newG     = color.g;
	f32 newB     = color.b;

	u32 src  = bitmapPtr[x + (y * pitchInU32)];
	f32 srcR = (f32)((src >> 16) & 0xFF);
	f32 srcG = (f32)((src >> 8) & 0xFF);
	f32 srcB = (f32)((src >> 0) & 0xFF);

	// NOTE(doyle): AlphaBlend equations is (alpha * new) + (1 - alpha) * src.
	// IMPORTANT(doyle): We pre-multiply so we can take out the (alpha * new)
	f32 invANorm  = 1 - newANorm;
	f32 destR = newR + (invANorm * srcR);
	f32 destG = newG + (invANorm * srcG);
	f32 destB = newB + (invANorm * srcB);

	DQN_ASSERT(destR >= 0);
	DQN_ASSERT(destG >= 0);
	DQN_ASSERT(destB >= 0);

	const f32 COLOR_EPSILON = 0.1f;
	if (destR > 255.0f)
	{
		DQN_ASSERT((destR - 255.0f) < COLOR_EPSILON);
		destR = 255;
	}

	if (destG > 255.0f)
	{
		DQN_ASSERT((destG - 255.0f) < COLOR_EPSILON);
		destG = 255;
	}

	if (destB > 255.0f)
	{
		DQN_ASSERT((destB - 255.0f) < COLOR_EPSILON);
		destB = 255;
	}

	u32 pixel = ((u32)(destR) << 16 |
	             (u32)(destG) << 8 |
	             (u32)(destB) << 0);
	bitmapPtr[x + (y * pitchInU32)] = pixel;

	globalDebug.setPixelsPerFrame++;
}

void DTRRender_Text(PlatformRenderBuffer *const renderBuffer,
                    const DTRFont font, DqnV2 pos, const char *const text,
                    DqnV4 color, i32 len)
{
	if (!text) return;
	if (!font.bitmap || !font.atlas || !renderBuffer) return;

	if (len == -1) len = Dqn_strlen(text);

	i32 index = 0;
	color     = DTRRender_PreMultiplyAlpha(color);
	while (index < len)
	{
		if (text[index] < font.codepointRange.min &&
		    text[index] > font.codepointRange.max)
		{
			return;
		}

		i32 charIndex = text[index++] - (i32)font.codepointRange.min;
		DQN_ASSERT(charIndex >= 0 &&
		           charIndex < (i32)(font.codepointRange.max -
		                             font.codepointRange.min));

		stbtt_aligned_quad alignedQuad = {};
		stbtt_GetPackedQuad(font.atlas, font.bitmapDim.w, font.bitmapDim.h,
		                    charIndex, &pos.x, &pos.y, &alignedQuad, true);

		DqnRect fontRect = {};
		fontRect.min     = DqnV2_2f(alignedQuad.s0 * font.bitmapDim.w, alignedQuad.t1 * font.bitmapDim.h);
		fontRect.max     = DqnV2_2f(alignedQuad.s1 * font.bitmapDim.w, alignedQuad.t0 * font.bitmapDim.h);

		DqnRect screenRect = {};
		screenRect.min     = DqnV2_2f(alignedQuad.x0, alignedQuad.y0);
		screenRect.max     = DqnV2_2f(alignedQuad.x1, alignedQuad.y1);

		// TODO: Assumes 1bpp and pitch of font bitmap
		const u32 fontPitch = font.bitmapDim.w;
		u32 fontOffset      = (u32)(fontRect.min.x + (fontRect.max.y * fontPitch));
		u8 *fontPtr         = font.bitmap + fontOffset;

		DQN_ASSERT(sizeof(u32) == renderBuffer->bytesPerPixel);

		// NOTE(doyle): This offset, yOffset and flipping t1, t0 is necessary
		// for reversing the order of the font since its convention is 0,0 top
		// left and -ve Y.
		stbtt_packedchar *const charData = font.atlas + charIndex;
		f32 fontHeightOffset             = charData->yoff2 + charData->yoff;

		u32 screenOffset = (u32)(screenRect.min.x + (screenRect.min.y - fontHeightOffset) * renderBuffer->width);
		u32 *screenPtr   = ((u32 *)renderBuffer->memory) + screenOffset;

		i32 fontWidth    = DQN_ABS((i32)(fontRect.min.x - fontRect.max.x));
		i32 fontHeight   = DQN_ABS((i32)(fontRect.min.y - fontRect.max.y));
		for (i32 y = 0; y < fontHeight; y++)
		{
			for (i32 x = 0; x < fontWidth; x++)
			{
				i32 yOffset = fontHeight - y;
				u8 srcA     = fontPtr[x + (yOffset * fontPitch)];
				if (srcA == 0) continue;

				f32 srcANorm = srcA / 255.0f;
				DqnV4 resultColor = {};
				resultColor.r     = color.r * srcANorm;
				resultColor.g     = color.g * srcANorm;
				resultColor.b     = color.b * srcANorm;
				resultColor.a     = color.a * srcANorm;

				i32 actualX = (i32)(screenRect.min.x + x);
				i32 actualY = (i32)(screenRect.min.y + y - fontHeightOffset);
				SetPixel(renderBuffer, actualX, actualY, resultColor);
			}
		}
	}
}

FILE_SCOPE void TransformPoints(const DqnV2 origin, DqnV2 *const pList,
                                const i32 numP, const DqnV2 scale,
                                const f32 rotation)
{
	if (!pList || numP == 0) return;

	DqnV2 xAxis = (DqnV2_2f(cosf(rotation), sinf(rotation)));
	DqnV2 yAxis = DqnV2_2f(-xAxis.y, xAxis.x);
	xAxis *= scale.x;
	yAxis *= scale.y;

	for (i32 i = 0; i < numP; i++)
	{
		DqnV2 p  = pList[i];
		pList[i] = origin + (xAxis * p.x) + (yAxis * p.y);
	}
}

void DTRRender_Line(PlatformRenderBuffer *const renderBuffer, DqnV2i a,
                    DqnV2i b, DqnV4 color)
{
	if (!renderBuffer) return;
	color = DTRRender_PreMultiplyAlpha(color);

	bool yTallerThanX = false;
	if (DQN_ABS(a.x - b.x) < DQN_ABS(a.y - b.y))
	{
		// NOTE(doyle): Enforce that the X component is always longer than the
		// Y component. When drawing this we just reverse the order back.
		// This is to ensure that the gradient is always < 1, such that we can
		// use the gradient to calculate the distance from the pixel origin, and
		// at which point we want to increment the y.
		yTallerThanX = true;
		DQN_SWAP(i32, a.x, a.y);
		DQN_SWAP(i32, b.x, b.y);
	}

	if (b.x < a.x) DQN_SWAP(DqnV2i, a, b);

	i32 rise = b.y - a.y;
	i32 run  = b.x - a.x;

	i32 delta         = (b.y > a.y) ? 1 : -1;
	i32 numIterations = b.x - a.x;

	i32 distFromPixelOrigin = DQN_ABS(rise) * 2;
	i32 distAccumulator     = 0;

	i32 newX = a.x;
	i32 newY = a.y;

	// Unflip the points if we did for plotting the pixels
	i32 *plotX, *plotY;
	if (yTallerThanX)
	{
		plotX = &newY;
		plotY = &newX;
	}
	else
	{
		plotX = &newX;
		plotY = &newY;
	}

	for (i32 iterateX = 0; iterateX < numIterations; iterateX++)
	{
		newX = a.x + iterateX;
		SetPixel(renderBuffer, *plotX, *plotY, color);

		distAccumulator += distFromPixelOrigin;
   		if (distAccumulator > run)
		{
			newY += delta;
			distAccumulator -= (run * 2);
		}
	}
}

// NOTE: This information is only particularly relevant for bitmaps so that
// after transformation, we can still programatically find the original
// coordinate system of the bitmap for texture mapping.
enum RectPointsIndex
{
	RectPointsIndex_Basis = 0,
	RectPointsIndex_XAxis,
	RectPointsIndex_Point,
	RectPointsIndex_YAxis,
	RectPointsIndex_Count
};

typedef struct RectPoints
{
	DqnV2 pList[RectPointsIndex_Count];
} RectPoints;

// Apply rotation and scale around the anchored point. This is a helper function that expands the
// min and max into the 4 vertexes of a rectangle then calls the normal transform routine.
// anchor: A normalised [0->1] value the points should be positioned from
FILE_SCOPE RectPoints TransformRectPoints(DqnV2 min, DqnV2 max, DqnV2 anchor, DqnV2 scale,
                                          f32 rotation)
{
	DqnV2 dim    = DqnV2_2f(max.x - min.x, max.y - min.y);
	DqnV2 origin = DqnV2_2f(min.x + (anchor.x * dim.w), min.y + (anchor.y * dim.h));
	DQN_ASSERT(dim.w > 0 && dim.h > 0);

	RectPoints result = {};
	result.pList[RectPointsIndex_Basis] = min - origin;
	result.pList[RectPointsIndex_XAxis]       = DqnV2_2f(max.x, min.y) - origin;
	result.pList[RectPointsIndex_Point]       = max - origin;
	result.pList[RectPointsIndex_YAxis]       = DqnV2_2f(min.x, max.y) - origin;

	TransformPoints(origin, result.pList, DQN_ARRAY_COUNT(result.pList), scale, rotation);

	return result;
}

FILE_SCOPE DqnRect GetBoundingBox(const DqnV2 *const pList, const i32 numP)
{
	DqnRect result = {};
	if (numP == 0 || !pList) return result;

	result.min = pList[0];
	result.max = pList[0];
	for (i32 i = 1; i < numP; i++)
	{
		DqnV2 checkP = pList[i];
		result.min.x = DQN_MIN(result.min.x, checkP.x);
		result.min.y = DQN_MIN(result.min.y, checkP.y);

		result.max.x = DQN_MAX(result.max.x, checkP.x);
		result.max.y = DQN_MAX(result.max.y, checkP.y);
	}

	return result;
}

void DTRRender_Rectangle(PlatformRenderBuffer *const renderBuffer, DqnV2 min,
                         DqnV2 max, DqnV4 color, const DqnV2 scale,
                         const f32 rotation, const DqnV2 anchor)
{
	////////////////////////////////////////////////////////////////////////////
	// Transform vertexes
	////////////////////////////////////////////////////////////////////////////
	color = DTRRender_PreMultiplyAlpha(color);

	RectPoints rectPoints     = TransformRectPoints(min, max, anchor, scale, rotation);
	DqnV2 *const pList        = &rectPoints.pList[0];
	const i32 RECT_PLIST_SIZE = DQN_ARRAY_COUNT(rectPoints.pList);

	DqnRect bounds = GetBoundingBox(pList, RECT_PLIST_SIZE);
	min = bounds.min;
	max = bounds.max;

	////////////////////////////////////////////////////////////////////////////
	// Clip Drawing Space
	////////////////////////////////////////////////////////////////////////////
	DqnRect rect = DqnRect_4f(min.x, min.y, max.x, max.y);
	DqnRect clip = DqnRect_4i(0, 0, renderBuffer->width, renderBuffer->height);

	DqnRect clippedRect = DqnRect_ClipRect(rect, clip);
	DqnV2 clippedSize  = DqnRect_GetSizeV2(clippedRect);

	////////////////////////////////////////////////////////////////////////////
	// Render
	////////////////////////////////////////////////////////////////////////////
	if (rotation != 0)
	{
		for (i32 y = 0; y < clippedSize.w; y++)
		{
			i32 bufferY = (i32)clippedRect.min.y + y;
			for (i32 x = 0; x < clippedSize.h; x++)
			{
				i32 bufferX = (i32)clippedRect.min.x + x;
				bool pIsInside = true;

				for (i32 pIndex = 0; pIndex < RECT_PLIST_SIZE; pIndex++)
				{
					DqnV2 origin  = pList[pIndex];
					DqnV2 line    = pList[(pIndex + 1) % RECT_PLIST_SIZE] - origin;
					DqnV2 axis    = DqnV2_2i(bufferX, bufferY) - origin;
					f32 dotResult = DqnV2_Dot(line, axis);

					if (dotResult < 0)
					{
						pIsInside = false;
						break;
					}
				}

				if (pIsInside) SetPixel(renderBuffer, bufferX, bufferY, color);
			}
		}
	}
	else
	{
		for (i32 y = 0; y < clippedSize.h; y++)
		{
			i32 bufferY = (i32)clippedRect.min.y + y;
			for (i32 x = 0; x < clippedSize.w; x++)
			{
				i32 bufferX = (i32)clippedRect.min.x + x;
				SetPixel(renderBuffer, bufferX, bufferY, color);
			}
		}
	}

	////////////////////////////////////////////////////////////////////////////
	// Debug
	////////////////////////////////////////////////////////////////////////////
	if (DTR_DEBUG)
	{
		// Draw Bounding box
		{
			DTRRender_Line(renderBuffer, DqnV2i_2f(min.x, min.y), DqnV2i_2f(min.x, max.y), color);
			DTRRender_Line(renderBuffer, DqnV2i_2f(min.x, max.y), DqnV2i_2f(max.x, max.y), color);
			DTRRender_Line(renderBuffer, DqnV2i_2f(max.x, max.y), DqnV2i_2f(max.x, min.y), color);
			DTRRender_Line(renderBuffer, DqnV2i_2f(max.x, min.y), DqnV2i_2f(min.x, min.y), color);
		}

		// Draw rotating outline
		if (rotation > 0)
		{
			DqnV4 green = DqnV4_4f(0, 255, 0, 255);
			DTRRender_Line(renderBuffer, DqnV2i_V2(pList[0]), DqnV2i_V2(pList[1]), green);
			DTRRender_Line(renderBuffer, DqnV2i_V2(pList[1]), DqnV2i_V2(pList[2]), green);
			DTRRender_Line(renderBuffer, DqnV2i_V2(pList[2]), DqnV2i_V2(pList[3]), green);
			DTRRender_Line(renderBuffer, DqnV2i_V2(pList[3]), DqnV2i_V2(pList[0]), green);
		}

	}
}

void DTRRender_Triangle(PlatformRenderBuffer *const renderBuffer, DqnV2 p1,
                        DqnV2 p2, DqnV2 p3, DqnV4 color, const DqnV2 scale,
                        const f32 rotation, const DqnV2 anchor)
{
	////////////////////////////////////////////////////////////////////////////
	// Transform vertexes
	////////////////////////////////////////////////////////////////////////////
	DqnV2 p1p2         = p2 - p1;
	DqnV2 p1p3         = p3 - p1;
	DqnV2 p1p2Anchored = p1p2 * anchor;
	DqnV2 p1p3Anchored = p1p3 * anchor;

	DqnV2 origin   = p1 + p1p2Anchored + p1p3Anchored;
	DqnV2 pList[3] = {p1 - origin, p2 - origin, p3 - origin};
	TransformPoints(origin, pList, DQN_ARRAY_COUNT(pList), scale, rotation);
	p1 = pList[0];
	p2 = pList[1];
	p3 = pList[2];

	color      = DTRRender_PreMultiplyAlpha(color);

	////////////////////////////////////////////////////////////////////////////
	// Calculate Bounding Box
	////////////////////////////////////////////////////////////////////////////
	DqnV2i max = DqnV2i_2f(DQN_MAX(DQN_MAX(p1.x, p2.x), p3.x),
	                       DQN_MAX(DQN_MAX(p1.y, p2.y), p3.y));
	DqnV2i min = DqnV2i_2f(DQN_MIN(DQN_MIN(p1.x, p2.x), p3.x),
	                       DQN_MIN(DQN_MIN(p1.y, p2.y), p3.y));
	min.x = DQN_MAX(min.x, 0);
	min.y = DQN_MAX(min.y, 0);
	max.x = DQN_MIN(max.x, renderBuffer->width - 1);
	max.y = DQN_MIN(max.y, renderBuffer->height - 1);

	/*
	   /////////////////////////////////////////////////////////////////////////
	   // Rearranging the Determinant
	   /////////////////////////////////////////////////////////////////////////
	   Given two points that form a line and an extra point to test, we can
	   determine whether a point lies on the line, or is to the left or right of
	   a the line.

	   First forming a 3x3 matrix of our terms with a, b being from the triangle
	   and test point c, we can derive a 2x2 matrix by subtracting the 1st
	   column from the 2nd and 1st column from the third.

	       | ax bx cx |     | (bx - ax)  (cx - ax) |
	   m = | ay by cy | ==> | (by - ay)  (cy - ay) |
	       | 1  1  1  |

	   From our 2x2 representation we can calculate the determinant which gives
	   us the signed area of the triangle extended into a parallelogram.

	   det(m) = (bx - ax)(cy - ay) - (by - ay)(cx - ax)

	   Depending on the order of the vertices supplied, if it's
	   - CCW and c(x,y) is outside the line (triangle), the signed area is negative
	   - CCW and c(x,y) is inside  the line (triangle), the signed area is positive
	   - CW  and c(x,y) is outside the line (triangle), the signed area is positive
	   - CW  and c(x,y) is inside  the line (triangle), the signed area is negative

	   /////////////////////////////////////////////////////////////////////////
	   // Optimising the Determinant Calculation
	   /////////////////////////////////////////////////////////////////////////
	   The det(m) can be rearranged if expanded to be
	   SignedArea(cx, cy) = (ay - by)cx + (bx - ay)cy + (ax*by - ay*bx)

	   When we scan to fill our triangle we go pixel by pixel, left to right,
	   bottom to top, notice that this translates to +1 for x and +1 for y, i.e.

	   The first pixel's signed area is cx, then cx+1, cx+2 .. etc
	   SignedArea(cx, cy)   = (ay - by)cx   + (bx - ax)cy + (ax*by - ay*bx)
	   SignedArea(cx+1, cy) = (ay - by)cx+1 + (bx - ax)cy + (ax*by - ay*bx)

	   Then
	   SignedArea(cx+1, cy) - SignedArea(cx, cy) =
	     (ay - by)cx+1 + (bx - ax)cy + (ax*by - ay*bx)
	   - (ay - by)cx   + (bx - ax)cy + (ax*by - ay*bx)
	   = (ay - by)cx+1 - (ay - by)cx
	   = (ay - by)(cx+1 - cx)
	   = (ay - by)(1)         = (ay - by)

	   Similarly when progressing in y
	   SignedArea(cx, cy)   = (ay - by)cx + (bx - ay)cy   + (ax*by - ay*bx)
	   SignedArea(cx, cy+1) = (ay - by)cx + (bx - ay)cy+1 + (ax*by - ay*bx)

	   Then
	   SignedArea(cx, cy+1) - SignedArea(cx, cy) =
	     (ay - by)cx + (bx - ax)cy+1 + (ax*by - ay*bx)
	   - (ay - by)cx + (bx - ax)cy   + (ax*by - ay*bx)
	   = (bx - ax)cy+1 - (bx - ax)cy
	   = (bx - ax)(cy+1 - cy)
	   = (bx - ax)(1)         = (bx - ax)

	   Then we can see that when we progress along x, we only need to change by
	   the value of SignedArea by (ay - by) and similarly for y, (bx - ay)

	   /////////////////////////////////////////////////////////////////////////
	   // Barycentric Coordinates
	   /////////////////////////////////////////////////////////////////////////
	   At this point we have an equation that can be used to calculate the
	   2x the signed area of a triangle, or the signed area of a parallelogram,
	   the two of which are equivalent.

	   det(m)             = (bx - ax)(cy - ay) - (by - ay)(cx - ax)
	   SignedArea(cx, cy) = (ay - by)cx + (bx - ay)cy + (ax*by - ay*bx)

	   A barycentric coordinate is some coefficient on A, B, C that allows us to
	   specify an arbitrary point in the triangle as a linear combination of the
	   three usually with some coefficient [0, 1].

	   The SignedArea turns out to be actually the barycentric coord for c(x, y)
	   normalised to the sum of the parallelogram area. For example a triangle
	   with points, A, B, C and an arbitrary point P inside the triangle. Then

	   SignedArea(P) with vertex A and B = Barycentric Coordinate for C
	   SignedArea(P) with vertex B and C = Barycentric Coordinate for A
	   SignedArea(P) with vertex C and A = Barycentric Coordinate for B

	       B
	      / \
	     /   \
	    /  P  \
	   /_______\
	  A        C

	   This is normalised to the area's sum, but we can trivially turn this into
	   a normalised version by dividing the area of the parallelogram, i.e.

	   BaryCentricC(P) = (SignedArea(P) with vertex A and B)/SignedArea(with the orig triangle vertex)
	   BaryCentricA(P) = (SignedArea(P) with vertex B and C)/SignedArea(with the orig triangle vertex)
	   BaryCentricB(P) = (SignedArea(P) with vertex C and A)/SignedArea(with the orig triangle vertex)
	*/

	f32 area2Times = ((p2.x - p1.x) * (p2.y + p1.y)) +
	                 ((p3.x - p2.x) * (p3.y + p2.y)) +
	                 ((p1.x - p3.x) * (p1.y + p3.y));
	if (area2Times > 0)
	{
		// Clockwise swap any point to make it clockwise
		DQN_SWAP(DqnV2, p2, p3);
	}

	const DqnV2 a = p1;
	const DqnV2 b = p2;
	const DqnV2 c = p3;

	DqnV2i scanP          = DqnV2i_2i(min.x, min.y);
	f32 signedArea1       = ((b.x - a.x) * (scanP.y - a.y)) - ((b.y - a.y) * (scanP.x - a.x));
	f32 signedArea1DeltaX = a.y - b.y;
	f32 signedArea1DeltaY = b.x - a.x;

	f32 signedArea2       = ((c.x - b.x) * (scanP.y - b.y)) - ((c.y - b.y) * (scanP.x - b.x));
	f32 signedArea2DeltaX = b.y - c.y;
	f32 signedArea2DeltaY = c.x - b.x;

	f32 signedArea3       = ((a.x - c.x) * (scanP.y - c.y)) - ((a.y - c.y) * (scanP.x - c.x));
	f32 signedArea3DeltaX = c.y - a.y;
	f32 signedArea3DeltaY = a.x - c.x;

	f32 invSignedAreaParallelogram = 1 / ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));

	////////////////////////////////////////////////////////////////////////////
	// Scan and Render
	////////////////////////////////////////////////////////////////////////////
	for (scanP.y = min.y; scanP.y < max.y; scanP.y++)
	{

		f32 signedArea1Row = signedArea1;
		f32 signedArea2Row = signedArea2;
		f32 signedArea3Row = signedArea3;

		for (scanP.x = min.x; scanP.x < max.x; scanP.x++)
		{
			if (signedArea1Row >= 0 && signedArea2Row >= 0 && signedArea3Row >= 0)
			{
				SetPixel(renderBuffer, scanP.x, scanP.y, color);
			}

			signedArea1Row += signedArea1DeltaX;
			signedArea2Row += signedArea2DeltaX;
			signedArea3Row += signedArea3DeltaX;
		}

		signedArea1 += signedArea1DeltaY;
		signedArea2 += signedArea2DeltaY;
		signedArea3 += signedArea3DeltaY;
	}

	////////////////////////////////////////////////////////////////////////////
	// Debug
	////////////////////////////////////////////////////////////////////////////
	if (DTR_DEBUG)
	{
		// Draw Bounding box
		{
			DTRRender_Line(renderBuffer, DqnV2i_2i(min.x, min.y), DqnV2i_2i(min.x, max.y), color);
			DTRRender_Line(renderBuffer, DqnV2i_2i(min.x, max.y), DqnV2i_2i(max.x, max.y), color);
			DTRRender_Line(renderBuffer, DqnV2i_2i(max.x, max.y), DqnV2i_2i(max.x, min.y), color);
			DTRRender_Line(renderBuffer, DqnV2i_2i(max.x, min.y), DqnV2i_2i(min.x, min.y), color);
		}

		// Draw Triangle Coordinate Basis
		{
			DqnV2 xAxis = DqnV2_2f(cosf(rotation), sinf(rotation)) * scale.x;
			DqnV2 yAxis = DqnV2_2f(-xAxis.y, xAxis.x) * scale.y;
			DqnV4 coordSysColor = DqnV4_4f(0, 255, 255, 255);
			i32 axisLen = 50;
			DTRRender_Line(renderBuffer, DqnV2i_V2(origin), DqnV2i_V2(origin) + DqnV2i_V2(xAxis * axisLen), coordSysColor);
			DTRRender_Line(renderBuffer, DqnV2i_V2(origin), DqnV2i_V2(origin) + DqnV2i_V2(yAxis * axisLen), coordSysColor);
		}

		// Draw axis point
		{
			DqnV4 green  = DqnV4_4f(0, 255, 0, 255);
			DqnV4 blue   = DqnV4_4f(0, 0, 255, 255);
			DqnV4 purple = DqnV4_4f(255, 0, 255, 255);

			DTRRender_Rectangle(renderBuffer, p1 - DqnV2_1f(5), p1 + DqnV2_1f(5), green);
			DTRRender_Rectangle(renderBuffer, p2 - DqnV2_1f(5), p2 + DqnV2_1f(5), blue);
			DTRRender_Rectangle(renderBuffer, p3 - DqnV2_1f(5), p3 + DqnV2_1f(5), purple);
		}
	}
}

void DTRRender_Bitmap(PlatformRenderBuffer *const renderBuffer,
                      DTRBitmap *const bitmap, DqnV2 pos,
                      DTRRenderTransform transform)
{
	if (!bitmap || !bitmap->memory || !renderBuffer) return;

	////////////////////////////////////////////////////////////////////////////
	// Transform vertexes
	////////////////////////////////////////////////////////////////////////////
	DqnV2 min = pos;
	DqnV2 max = min + DqnV2_V2i(bitmap->dim);
	DTRDebug_PushText("OldRect: (%5.2f, %5.2f), (%5.2f, %5.2f)", min.x, min.y, max.x, max.y);

	RectPoints rectPoints     = TransformRectPoints(min, max, transform.anchor, transform.scale, transform.rotation);
	const DqnV2 *const pList  = &rectPoints.pList[0];
	const i32 RECT_PLIST_SIZE = DQN_ARRAY_COUNT(rectPoints.pList);

	DqnRect bounds = GetBoundingBox(pList, RECT_PLIST_SIZE);
	min = bounds.min;
	max = bounds.max;

	////////////////////////////////////////////////////////////////////////////
	// Clip drawing space
	////////////////////////////////////////////////////////////////////////////
	DqnRect drawRect = DqnRect_4f(bounds.min.x, bounds.min.y, bounds.max.x, bounds.max.y);
	DqnRect clip     = DqnRect_4i(0, 0, renderBuffer->width, renderBuffer->height);

	DqnRect clippedDrawRect = DqnRect_ClipRect(drawRect, clip);
	DqnV2 clippedSize       = DqnRect_GetSizeV2(clippedDrawRect);

	DTRDebug_PushText("ClippedRect: (%5.2f, %5.2f), (%5.2f, %5.2f)", clippedDrawRect.min.x, clippedDrawRect.min.y, clippedDrawRect.max.x, clippedDrawRect.max.y);
	DTRDebug_PushText("ClippedSize: (%5.2f, %5.2f)", clippedSize.w, clippedSize.h);
	DTRDebug_PushText("DrawRect: (%5.2f, %5.2f), (%5.2f, %5.2f)", drawRect.min.x, drawRect.min.y, drawRect.max.x, drawRect.max.y);
	const i32 pitch      = bitmap->dim.w * bitmap->bytesPerPixel;
	u8 *const bitmapPtr  = (u8 *)bitmap->memory;

	////////////////////////////////////////////////////////////////////////////
	// Setup Texture Mapping
	////////////////////////////////////////////////////////////////////////////
	const DqnV2 rectBasis       = pList[RectPointsIndex_Basis];
	const DqnV2 xAxisRelToBasis = pList[RectPointsIndex_XAxis] - rectBasis;
	const DqnV2 yAxisRelToBasis = pList[RectPointsIndex_YAxis] - rectBasis;

	const f32 invXAxisLenSq = 1 / DqnV2_LengthSquared(DqnV2_1f(0), xAxisRelToBasis);
	const f32 invYAxisLenSq = 1 / DqnV2_LengthSquared(DqnV2_1f(0), yAxisRelToBasis);
	for (i32 y = 0; y < (i32)clippedSize.h; y++)
	{
		const i32 bufferY = (i32)clippedDrawRect.min.y + y;
		for (i32 x = 0; x < (i32)clippedSize.w; x++)
		{
			const i32 bufferX = (i32)clippedDrawRect.min.x + x;

			bool bufXYIsInside = true;
			for (i32 pIndex = 0; pIndex < RECT_PLIST_SIZE; pIndex++)
			{
				DqnV2 origin = pList[pIndex];
				DqnV2 axis   = pList[(pIndex + 1) % RECT_PLIST_SIZE] - origin;
				DqnV2 testP  = DqnV2_2i(bufferX, bufferY)            - origin;

				f32 dot = DqnV2_Dot(testP, axis);
				if (dot < 0)
				{
					bufXYIsInside = false;
					break;
				}
			}

			if (bufXYIsInside)
			{
#if 0
				u32 *pixelPtr = (u32 *)srcRow;
				u32 pixel     = 0xFFFFFFFF; // pixelPtr[texelX + x];

				DqnV4 color = {};
				color.a     = (f32)(pixel >> 24);
				color.b     = (f32)((pixel >> 16) & 0xFF);
				color.g     = (f32)((pixel >> 8) & 0xFF);
				color.r     = (f32)((pixel >> 0) & 0xFF);

				SetPixel(renderBuffer, bufferX, bufferY, color);
#else
				DqnV2 bufPRelToBasis = DqnV2_2i(bufferX, bufferY) - rectBasis;

				f32 u = DqnV2_Dot(bufPRelToBasis, xAxisRelToBasis) * invXAxisLenSq;
				f32 v = DqnV2_Dot(bufPRelToBasis, yAxisRelToBasis) * invYAxisLenSq;
				u     = DqnMath_Clampf(u, 0.0f, 1.0f);
				v     = DqnMath_Clampf(v, 0.0f, 1.0f);

				f32 texelXf = u * (f32)(bitmap->dim.w - 1);
				f32 texelYf = v * (f32)(bitmap->dim.h - 1);
				DQN_ASSERT(texelXf >= 0 && texelXf < bitmap->dim.w);
				DQN_ASSERT(texelYf >= 0 && texelYf < bitmap->dim.h);

				i32 texelX           = (i32)texelXf;
				i32 texelY           = (i32)texelYf;
				f32 texelFractionalX = texelXf - texelX;
				f32 texelFractionalY = texelYf - texelY;

				i32 texel1X = texelX;
				i32 texel1Y = texelY;

				i32 texel2X = DQN_MIN((texelX + 1), bitmap->dim.w - 1);
				i32 texel2Y = texelY;

				i32 texel3X = texelX;
				i32 texel3Y = DQN_MIN((texelY + 1), bitmap->dim.h - 1);

				i32 texel4X = DQN_MIN((texelX + 1), bitmap->dim.w - 1);
				i32 texel4Y = DQN_MIN((texelY + 1), bitmap->dim.h - 1);

				u32 texel1  = *(u32 *)(bitmapPtr + ((texel1X * bitmap->bytesPerPixel) + (texel1Y * pitch)));
				u32 texel2  = *(u32 *)(bitmapPtr + ((texel2X * bitmap->bytesPerPixel) + (texel2Y * pitch)));
				u32 texel3  = *(u32 *)(bitmapPtr + ((texel3X * bitmap->bytesPerPixel) + (texel3Y * pitch)));
				u32 texel4  = *(u32 *)(bitmapPtr + ((texel4X * bitmap->bytesPerPixel) + (texel4Y * pitch)));

				DqnV4 color1;
				color1.a      = (f32)(texel1 >> 24);
				color1.b      = (f32)((texel1 >> 16) & 0xFF);
				color1.g      = (f32)((texel1 >> 8) & 0xFF);
				color1.r      = (f32)((texel1 >> 0) & 0xFF);

				DqnV4 color2;
				color2.a      = (f32)(texel2 >> 24);
				color2.b      = (f32)((texel2 >> 16) & 0xFF);
				color2.g      = (f32)((texel2 >> 8) & 0xFF);
				color2.r      = (f32)((texel2 >> 0) & 0xFF);

				DqnV4 color3;
				color3.a      = (f32)(texel3 >> 24);
				color3.b      = (f32)((texel3 >> 16) & 0xFF);
				color3.g      = (f32)((texel3 >> 8) & 0xFF);
				color3.r      = (f32)((texel3 >> 0) & 0xFF);

				DqnV4 color4;
				color4.a      = (f32)(texel4 >> 24);
				color4.b      = (f32)((texel4 >> 16) & 0xFF);
				color4.g      = (f32)((texel4 >> 8) & 0xFF);
				color4.r      = (f32)((texel4 >> 0) & 0xFF);

				DqnV4 color12;
				color12.a = DqnMath_Lerp(color1.a, texelFractionalX, color2.a);
				color12.b = DqnMath_Lerp(color1.b, texelFractionalX, color2.b);
				color12.g = DqnMath_Lerp(color1.g, texelFractionalX, color2.g);
				color12.r = DqnMath_Lerp(color1.r, texelFractionalX, color2.r);

				DqnV4 color34;
				color34.a = DqnMath_Lerp(color3.a, texelFractionalX, color4.a);
				color34.b = DqnMath_Lerp(color3.b, texelFractionalX, color4.b);
				color34.g = DqnMath_Lerp(color3.g, texelFractionalX, color4.g);
				color34.r = DqnMath_Lerp(color3.r, texelFractionalX, color4.r);

				DqnV4 color;
				color.a = DqnMath_Lerp(color12.a, texelFractionalY, color34.a);
				color.b = DqnMath_Lerp(color12.b, texelFractionalY, color34.b);
				color.g = DqnMath_Lerp(color12.g, texelFractionalY, color34.g);
				color.r = DqnMath_Lerp(color12.r, texelFractionalY, color34.r);

				SetPixel(renderBuffer, bufferX, bufferY, color);
#endif
			}
		}
	}

	if (DTR_DEBUG)
	{
		// Draw Bounding box
		{
			DqnV4 yellow = DqnV4_4f(255, 255, 0, 255);
			DTRRender_Line(renderBuffer, DqnV2i_2f(min.x, min.y), DqnV2i_2f(min.x, max.y), yellow);
			DTRRender_Line(renderBuffer, DqnV2i_2f(min.x, max.y), DqnV2i_2f(max.x, max.y), yellow);
			DTRRender_Line(renderBuffer, DqnV2i_2f(max.x, max.y), DqnV2i_2f(max.x, min.y), yellow);
			DTRRender_Line(renderBuffer, DqnV2i_2f(max.x, min.y), DqnV2i_2f(min.x, min.y), yellow);
		}

		// Draw rotating outline
		if (transform.rotation > 0)
		{
			DqnV4 green = DqnV4_4f(0, 255, 0, 255);
			DTRRender_Line(renderBuffer, DqnV2i_V2(pList[0]), DqnV2i_V2(pList[1]), green);
			DTRRender_Line(renderBuffer, DqnV2i_V2(pList[1]), DqnV2i_V2(pList[2]), green);
			DTRRender_Line(renderBuffer, DqnV2i_V2(pList[2]), DqnV2i_V2(pList[3]), green);
			DTRRender_Line(renderBuffer, DqnV2i_V2(pList[3]), DqnV2i_V2(pList[0]), green);
		}

		// Draw axis point
		{
			DqnV4 red    = DqnV4_4f(255, 0, 0, 255);
			DqnV4 green  = DqnV4_4f(0, 255, 0, 255);
			DqnV4 blue   = DqnV4_4f(0, 0, 255, 255);
			DqnV4 purple = DqnV4_4f(255, 0, 255, 255);

			DqnV2 p1 = pList[0];
			DqnV2 p2 = pList[1];
			DqnV2 p3 = pList[2];
			DqnV2 p4 = pList[3];
			DTRRender_Rectangle(renderBuffer, p1 - DqnV2_1f(5), p1 + DqnV2_1f(5), green);
			DTRRender_Rectangle(renderBuffer, p2 - DqnV2_1f(5), p2 + DqnV2_1f(5), blue);
			DTRRender_Rectangle(renderBuffer, p3 - DqnV2_1f(5), p3 + DqnV2_1f(5), purple);
			DTRRender_Rectangle(renderBuffer, p4 - DqnV2_1f(5), p4 + DqnV2_1f(5), red);
		}
	}
}

void DTRRender_Clear(PlatformRenderBuffer *const renderBuffer,
                     const DqnV3 color)
{
	if (!renderBuffer) return;

	DQN_ASSERT(color.r >= 0.0f && color.r <= 255.0f);
	DQN_ASSERT(color.g >= 0.0f && color.g <= 255.0f);
	DQN_ASSERT(color.b >= 0.0f && color.b <= 255.0f);

	u32 *const bitmapPtr = (u32 *)renderBuffer->memory;
	for (i32 y = 0; y < renderBuffer->height; y++)
	{
		for (i32 x = 0; x < renderBuffer->width; x++)
		{
			u32 pixel = ((i32)color.r << 16) | ((i32)color.g << 8) |
			            ((i32)color.b << 0);
			bitmapPtr[x + (y * renderBuffer->width)] = pixel;
		}
	}
}


