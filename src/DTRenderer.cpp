#include "DTRenderer.h"
#include "DTRendererDebug.h"
#include "DTRendererPlatform.h"
#include "DTRendererRender.h"

#define STB_IMAGE_IMPLEMENTATION
#include "external/stb_image.h"

#define DQN_IMPLEMENTATION
#include "dqn.h"

#include <math.h>

typedef struct WavefrontModelFace
{
	DqnArray<i32> vertexArray;
	DqnArray<i32> textureArray;
	DqnArray<i32> normalArray;
} WavefrontModelFace;

FILE_SCOPE inline WavefrontModelFace ObjWavefrontModelFaceInit(i32 capacity = 3)
{
	WavefrontModelFace result = {};
	DQN_ASSERT(DqnArray_Init(&result.vertexArray,  capacity));
	DQN_ASSERT(DqnArray_Init(&result.textureArray, capacity));
	DQN_ASSERT(DqnArray_Init(&result.normalArray,  capacity));

	return result;
}

typedef struct WavefrontModel
{
	// TODO(doyle): Fixed size
	char *groupName[16];
	i32 groupNameIndex;
	i32 groupSmoothing;

	DqnArray<WavefrontModelFace> faces;
} WavefrontModel;

typedef struct WavefrontObj
{
	DqnArray<DqnV4> geometryArray;
	DqnArray<DqnV3> texUVArray;
	DqnArray<DqnV3> normalArray;

	WavefrontModel model;
} WavefrontObj;

FILE_SCOPE bool ObjWaveFrontInit(WavefrontObj *const obj, const i32 vertexInitCapacity = 1000,
                                 const i32 faceInitCapacity = 200)
{
	if (!obj) return false;

	bool initialised = false;

	initialised |= DqnArray_Init(&obj->geometryArray, vertexInitCapacity);
	initialised |= DqnArray_Init(&obj->texUVArray,    vertexInitCapacity);
	initialised |= DqnArray_Init(&obj->normalArray,   vertexInitCapacity);
	initialised |= DqnArray_Init(&obj->model.faces,   faceInitCapacity);

	if (!initialised)
	{
		DqnArray_Free(&obj->geometryArray);
		DqnArray_Free(&obj->texUVArray);
		DqnArray_Free(&obj->normalArray);
		DqnArray_Free(&obj->model.faces);
	}

	return initialised;
}

FILE_SCOPE bool ObjWavefrontLoad(const PlatformAPI api, PlatformMemory *const memory,
                                 const char *const path)
{
	if (!memory || ! path) return false;

	PlatformFile file = {};
	if (!api.FileOpen(path, &file, PlatformFilePermissionFlag_Read))
		return false; // TODO(doyle): Logging

	DqnTempBuffer tmpMemRegion = DqnMemBuffer_BeginTempRegion(&memory->transientBuffer);
	u8 *rawBytes               = (u8 *)DqnMemBuffer_Allocate(&memory->transientBuffer, file.size);
	size_t bytesRead           = api.FileRead(&file, rawBytes, file.size);
	size_t fileSize            = file.size;
	api.FileClose(&file);
	if (bytesRead != file.size)
	{
		// TODO(doyle): Logging
		DqnMemBuffer_EndTempRegion(tmpMemRegion);
		return false;
	}

	enum WavefrontVertexType {
		WavefrontVertexType_Invalid,
		WavefrontVertexType_Geometric,
		WavefrontVertexType_Texture,
		WavefrontVertexType_Normal,
	};

	WavefrontObj obj = {};
	if (!ObjWaveFrontInit(&obj))
	{
		DqnMemBuffer_EndTempRegion(tmpMemRegion);
		return false;
	}

	for (char *scan = (char *)rawBytes; scan && scan < ((char *)rawBytes + fileSize);)
	{
		switch (DqnChar_ToLower(*scan))
		{
			////////////////////////////////////////////////////////////////////
			// Polygonal Free Form Statement
			////////////////////////////////////////////////////////////////////
			// Vertex Format: v[ |t|n|p] x y z [w]
			case 'v':
			{
				scan++;
				DQN_ASSERT(scan);

				enum WavefrontVertexType type = WavefrontVertexType_Invalid;

				char identifier = DqnChar_ToLower(*scan);
				if      (identifier == ' ') type = WavefrontVertexType_Geometric;
				else if (identifier == 't' || identifier == 'n')
				{
					scan++;
					if (identifier == 't') type = WavefrontVertexType_Texture;
					else                   type = WavefrontVertexType_Normal;
				}
				else DQN_ASSERT(DQN_INVALID_CODE_PATH);

				i32 vIndex = 0;
				DqnV4 v4   = {0, 0, 0, 1.0f};

				// Progress to first non space character after vertex identifier
				for (; scan && *scan == ' '; scan++)
					if (!scan) DQN_ASSERT(DQN_INVALID_CODE_PATH);

				for (;;)
				{
					char *f32StartPtr = scan;
					for (; *scan != ' ' && *scan != '\n';)
					{
						DQN_ASSERT(DqnChar_IsDigit(*scan) || (*scan == '.') || (*scan == '-') ||
						           *scan == 'e');
						scan++;
					}

					i32 f32Len = (i32)((size_t)scan - (size_t)f32StartPtr);
					v4.e[vIndex++] = Dqn_StrToF32(f32StartPtr, f32Len);
					DQN_ASSERT(vIndex < DQN_ARRAY_COUNT(v4.e));

					while (scan && (*scan == ' ' || *scan == '\n')) scan++;

					if (!scan) break;
					if (!(DqnChar_IsDigit(*scan) || *scan == '-')) break;
				}

				DQN_ASSERT(vIndex == 3 || vIndex == 4);
				if (type == WavefrontVertexType_Geometric)
				{
					DqnArray_Push(&obj.geometryArray, v4);
				}
				else if (type == WavefrontVertexType_Texture)
				{
					DqnArray_Push(&obj.texUVArray, v4.xyz);
				}
				else if (type == WavefrontVertexType_Normal)
				{
					DqnArray_Push(&obj.normalArray, v4.xyz);
				}
				else
				{
					DQN_ASSERT(DQN_INVALID_CODE_PATH);
				}
			}
			break;

			////////////////////////////////////////////////////////////////////
			// Polygonal Geometry
			////////////////////////////////////////////////////////////////////
			// Vertex numbers can be negative to reference a relative offset to
			// the vertex which means the relative order of the vertices
			// specified in the file, i.e.

			// v 0.000000 2.000000 2.000000
			// v 0.000000 0.000000 2.000000
			// v 2.000000 0.000000 2.000000
			// v 2.000000 2.000000 2.000000
			// f -4 -3 -2 -1

			// Point Format: p v1 v2 v3 ...
			// Each point is one vertex.
			case 'p':
			{
				DQN_ASSERT(DQN_INVALID_CODE_PATH);
			}
			break;

			// Line Format: l v1/vt1 v2/vt2 v3/vt3 ...
			// Texture vertex is optional. Minimum of two vertex numbers, no
			// limit on maximum.
			case 'l':
			{
				DQN_ASSERT(DQN_INVALID_CODE_PATH);
			}
			break;

			// Face Format: f v1/vt1/vn1 v2/vt2/vn2 v3/vt3/vn3 ...
			// Minimum of three vertexes, no limit on maximum. vt, vn are
			// optional. But specification of vt, vn must be consistent if given
			// across all the vertices for the line.

			// For example, to specify only the vertex and vertex normal
			// reference numbers, you would enter:
			
			// f 1//1 2//2 3//3 4//4
			case 'f':
			{
				scan++;
				while (scan && (*scan == ' ' || *scan == '\n')) scan++;
				if (!scan) continue;

				WavefrontModelFace face  = ObjWavefrontModelFaceInit();
				i32 numVertexesParsed    = 0;
				bool moreVertexesToParse = true;
				while (moreVertexesToParse)
				{
					enum WavefrontVertexType type = WavefrontVertexType_Geometric;

					// Read a vertexes 3 attributes v, vt, vn
					for (i32 i = 0; i < 3; i++)
					{
						char *numStartPtr = scan;
						while (scan && DqnChar_IsDigit(*scan))
							scan++;

						i32 numLen = (i32)((size_t)scan - (size_t)numStartPtr);
						if (numLen > 0)
						{
							i32 index = (i32)Dqn_StrToI64(numStartPtr, numLen);

							if (type == WavefrontVertexType_Geometric)
							{
								DQN_ASSERT(DqnArray_Push(&face.vertexArray, index));
							}
							else if (type == WavefrontVertexType_Texture)
							{
								DQN_ASSERT(DqnArray_Push(&face.textureArray, index));
							}
							else if (type == WavefrontVertexType_Normal)
							{
								DQN_ASSERT(DqnArray_Push(&face.normalArray, index));
							}
						}

						if (scan) scan++;
						type = (enum WavefrontVertexType)((i32)type + 1);
					}
					numVertexesParsed++;

					if (scan)
					{
						// Move to next "non-empty" character
						while (scan && (*scan == ' ' || *scan == '\n'))
							scan++;

						// If it isn't a digit, then we've read all the
						// vertexes for this face
						if (!scan || (scan && !DqnChar_IsDigit(*scan)))
						{
							moreVertexesToParse = false;
						}
					}

					if (obj.model.faces.count == 7470 || obj.model.faces.count == 2491)
					{
						int x = 5;
					}
				}
				DQN_ASSERT(numVertexesParsed >= 3);
				DQN_ASSERT(DqnArray_Push(&obj.model.faces, face));
			}
			break;

			////////////////////////////////////////////////////////////////////
			// Misc
			////////////////////////////////////////////////////////////////////

			// Group Name Format: g group_name1 group_name2
			// This is optional, if multiple groups are specified, then the
			// following elements belong to all groups. The default group name
			// is "default"
			case 'g':
			{
				scan++;
				while (scan && (*scan == ' ' || *scan == '\n')) scan++;

				if (!scan) continue;

				// Iterate to end of the name, i.e. move ptr to first space
				char *namePtr = scan;
				while (scan && (*scan != ' ' && *scan != '\n'))
					scan++;

				if (scan)
				{
					i32 nameLen = (i32)((size_t)scan - (size_t)namePtr);
					DQN_ASSERT(obj.model.groupNameIndex + 1 < DQN_ARRAY_COUNT(obj.model.groupName));

					DQN_ASSERT(!obj.model.groupName[obj.model.groupNameIndex]);
					obj.model.groupName[obj.model.groupNameIndex++] = (char *)DqnMemBuffer_Allocate(
					    &memory->permanentBuffer, (nameLen + 1) * sizeof(char));

					for (i32 i = 0; i < nameLen; i++)
						obj.model.groupName[obj.model.groupNameIndex - 1][i] = namePtr[i];

					while (scan && (*scan == ' ' || *scan == '\n'))
						scan++;
				}
			}
			break;

			// Smoothing Group: s group_number
			// Sets the smoothing group for the elements that follow it. If it's
			// not to be used it can be specified as "off" or a value of 0.
			case 's':
			{
				// Advance to first non space char after identifier
				scan++;
				while (scan && *scan == ' ' || *scan == '\n') scan++;

				if (scan && DqnChar_IsDigit(*scan))
				{
					char *numStartPtr = scan;
					while (scan && (*scan != ' ' && *scan != '\n'))
					{
						DQN_ASSERT(DqnChar_IsDigit(*scan));
						scan++;
					}

					i32 numLen               = (i32)((size_t)scan - (size_t)numStartPtr);
					i32 groupSmoothing       = (i32)Dqn_StrToI64(numStartPtr, numLen);
					obj.model.groupSmoothing = groupSmoothing;
				}

				while (scan && *scan == ' ' || *scan == '\n') scan++;
			}
			break;

			// Comment
			case '#':
			{
				// Skip comment line until new line
				while (scan && *scan != '\n')
					scan++;

				// Skip new lines and any leading white spaces
				while (scan && (*scan == '\n' || *scan == ' '))
					scan++;
			}
			break;

			default:
			{
				DQN_ASSERT(DQN_INVALID_CODE_PATH);
			}
			break;
		}
	}

	DqnMemBuffer_EndTempRegion(tmpMemRegion);

	return true;
}

FILE_SCOPE bool BitmapFontCreate(const PlatformAPI api,
                                 PlatformMemory *const memory,
                                 DTRFont *const font, const char *const path,
                                 const DqnV2i bitmapDim,
                                 const DqnV2i codepointRange,
                                 const f32 sizeInPt)
{
	if (!memory || !font || !path) return false;

	DTRFont loadedFont = {};
	loadedFont.bitmapDim      = bitmapDim;
	loadedFont.codepointRange = codepointRange;
	loadedFont.sizeInPt       = sizeInPt;

	////////////////////////////////////////////////////////////////////////////
	// Load font data
	////////////////////////////////////////////////////////////////////////////
	PlatformFile file = {};
	if (!api.FileOpen(path, &file, PlatformFilePermissionFlag_Read))
		return false; // TODO(doyle): Logging

	DqnTempBuffer tmpMemRegion = DqnMemBuffer_BeginTempRegion(&memory->transientBuffer);
	u8 *fontBuf                = (u8 *)DqnMemBuffer_Allocate(&memory->transientBuffer, file.size);
	size_t bytesRead           = api.FileRead(&file, fontBuf, file.size);
	api.FileClose(&file);
	if (bytesRead != file.size)
	{
		// TODO(doyle): Logging
		DqnMemBuffer_EndTempRegion(tmpMemRegion);
		return false;
	}

	stbtt_fontinfo fontInfo = {};
	if (stbtt_InitFont(&fontInfo, fontBuf, 0) == 0)
	{
		DQN_ASSERT(DQN_INVALID_CODE_PATH);
		return false;
	}

	if (DTR_DEBUG) DQN_ASSERT(stbtt_GetNumberOfFonts(fontBuf) == 1);
	////////////////////////////////////////////////////////////////////////////
	// Pack font data to bitmap
	////////////////////////////////////////////////////////////////////////////
	loadedFont.bitmap = (u8 *)DqnMemBuffer_Allocate(
	    &memory->permanentBuffer,
	    (size_t)(loadedFont.bitmapDim.w * loadedFont.bitmapDim.h));

	stbtt_pack_context fontPackContext = {};
	if (stbtt_PackBegin(&fontPackContext, loadedFont.bitmap, bitmapDim.w,
	                    bitmapDim.h, 0, 1, NULL) == 1)
	{
		// stbtt_PackSetOversampling(&fontPackContext, 2, 2);

		i32 numCodepoints =
		    (i32)((codepointRange.max + 1) - codepointRange.min);

		loadedFont.atlas = (stbtt_packedchar *)DqnMemBuffer_Allocate(
		    &memory->permanentBuffer, numCodepoints * sizeof(stbtt_packedchar));
		stbtt_PackFontRange(&fontPackContext, fontBuf, 0,
		                    STBTT_POINT_SIZE(sizeInPt), (i32)codepointRange.min,
		                    numCodepoints, loadedFont.atlas);
		stbtt_PackEnd(&fontPackContext);
		DqnMemBuffer_EndTempRegion(tmpMemRegion);
	}
	else
	{
		DQN_ASSERT(DQN_INVALID_CODE_PATH);
		DqnMemBuffer_EndTempRegion(tmpMemRegion);
		return false;
	}

	////////////////////////////////////////////////////////////////////////////
	// Premultiply Alpha of Bitmap
	////////////////////////////////////////////////////////////////////////////
	for (i32 y = 0; y < bitmapDim.h; y++)
	{
		for (i32 x = 0; x < bitmapDim.w; x++)
		{
			// NOTE: Bitmap from stb_truetype is 1BPP. So the actual color
			// value represents its' alpha value but also its' color.
			u32 index = x + (y * bitmapDim.w);
			f32 alpha = (f32)(loadedFont.bitmap[index]) / 255.0f;
			f32 color = alpha;

			color = DTRRender_SRGB1ToLinearSpacef(color);
			color = color * alpha;
			color = DTRRender_LinearToSRGB1Spacef(color) * 255.0f;
			DQN_ASSERT(color >= 0.0f && color <= 255.0f);

			loadedFont.bitmap[index] = (u8)color;
		}
	}

#ifdef DTR_DEBUG_RENDER_FONT_BITMAP
	stbi_write_bmp("test.bmp", bitmapDim.w, bitmapDim.h, 1, loadedFont.bitmap);
#endif

	*font = loadedFont;
	return true;
}

FILE_SCOPE bool BitmapLoad(const PlatformAPI api, DTRBitmap *bitmap,
                           const char *const path,
                           DqnMemBuffer *const transientBuffer)
{
	if (!bitmap) return false;

	PlatformFile file = {};
	if (!api.FileOpen(path, &file, PlatformFilePermissionFlag_Read))
		return false;

	DqnTempBuffer tempBuffer = DqnMemBuffer_BeginTempRegion(transientBuffer);
	{
		u8 *const rawData =
		    (u8 *)DqnMemBuffer_Allocate(transientBuffer, file.size);
		size_t bytesRead = api.FileRead(&file, rawData, file.size);
		api.FileClose(&file);

		if (bytesRead != file.size)
		{
			DqnMemBuffer_EndTempRegion(tempBuffer);
			return false;
		}

		bitmap->memory = stbi_load_from_memory(rawData, (i32)file.size, &bitmap->dim.w,
		                                       &bitmap->dim.h, &bitmap->bytesPerPixel, 4);
	}
	DqnMemBuffer_EndTempRegion(tempBuffer);
	if (!bitmap->memory) return false;

	const i32 pitch = bitmap->dim.w * bitmap->bytesPerPixel;
	for (i32 y = 0; y < bitmap->dim.h; y++)
	{
		u8 *const srcRow = bitmap->memory + (y * pitch);
		for (i32 x = 0; x < bitmap->dim.w; x++)
		{
			u32 *pixelPtr = (u32 *)srcRow;
			u32 pixel     = pixelPtr[x];

			DqnV4 color = {};
			color.a     = (f32)(pixel >> 24);
			color.b     = (f32)((pixel >> 16) & 0xFF);
			color.g     = (f32)((pixel >> 8) & 0xFF);
			color.r     = (f32)((pixel >> 0) & 0xFF);

			color *= DTRRENDER_INV_255;
			color = DTRRender_PreMultiplyAlphaSRGB1WithLinearConversion(color);
			color *= 255.0f;

			pixel = (((u32)color.a << 24) |
			         ((u32)color.b << 16) |
			         ((u32)color.g << 8) |
			         ((u32)color.r << 0));

			pixelPtr[x] = pixel;
		}
	}

	return true;
}

// #include <algorithm>
void CompAssignment(PlatformRenderBuffer *const renderBuffer, PlatformInput *const input,
                    PlatformMemory *const memory)
{
#if 1
	DqnV2 pList[] = {
	    DqnV2_2i(128, 6),   DqnV2_2i(869, 237), DqnV2_2i(318, 832), DqnV2_2i(765, 579),
	    DqnV2_2i(322, 531), DqnV2_2i(98, 226),  DqnV2_2i(366, 862), DqnV2_2i(976, 76),
	    DqnV2_2i(629, 210), DqnV2_2i(217, 741), DqnV2_2i(320, 263), DqnV2_2i(610, 587),
	    DqnV2_2i(842, 208), DqnV2_2i(106, 400), DqnV2_2i(142, 581), DqnV2_2i(591, 42),
	    DqnV2_2i(902, 296), DqnV2_2i(469, 865), DqnV2_2i(79, 517),  DqnV2_2i(481, 309),
	    DqnV2_2i(73, 940),  DqnV2_2i(525, 842), DqnV2_2i(765, 345), DqnV2_2i(94, 985),
	    DqnV2_2i(102, 572), DqnV2_2i(181, 380), DqnV2_2i(931, 186), DqnV2_2i(115, 864),
	    DqnV2_2i(426, 605), DqnV2_2i(340, 444), DqnV2_2i(796, 106), DqnV2_2i(108, 604),
	    DqnV2_2i(836, 540), DqnV2_2i(448, 837), DqnV2_2i(790, 335), DqnV2_2i(849, 411),
	    DqnV2_2i(569, 227), DqnV2_2i(54, 688),  DqnV2_2i(326, 303), DqnV2_2i(667, 85),
	    DqnV2_2i(894, 122), DqnV2_2i(207, 732), DqnV2_2i(259, 439), DqnV2_2i(403, 424),
	    DqnV2_2i(711, 511), DqnV2_2i(507, 826), DqnV2_2i(505, 141), DqnV2_2i(512, 552),
	    DqnV2_2i(244, 758), DqnV2_2i(867, 95),
	};
#else
	DqnV2 pList[] = {DqnV2_2i(815, 362), DqnV2_2i(558, 343), DqnV2_2i(124, 589), DqnV2_2i(433, 134), DqnV2_2i(547, 146), DqnV2_2i(394, 341), DqnV2_2i(730, 356),
	                 DqnV2_2i(146, 879), DqnV2_2i(74, 610),  DqnV2_2i(214, 610), DqnV2_2i(308, 487), DqnV2_2i(187, 371), DqnV2_2i(702, 315), DqnV2_2i(699, 11),
	                 DqnV2_2i(830, 341), DqnV2_2i(856, 493), DqnV2_2i(664, 19),  DqnV2_2i(819, 478), DqnV2_2i(265, 932), DqnV2_2i(489, 32),  DqnV2_2i(480, 439),
	                 DqnV2_2i(387, 913), DqnV2_2i(558, 279), DqnV2_2i(58, 718),  DqnV2_2i(90, 154),  DqnV2_2i(298, 79),  DqnV2_2i(715, 78),  DqnV2_2i(603, 32),
	                 DqnV2_2i(441, 367), DqnV2_2i(719, 67),  DqnV2_2i(7, 352),   DqnV2_2i(956, 184), DqnV2_2i(684, 336), DqnV2_2i(202, 323), DqnV2_2i(87, 586),
	                 DqnV2_2i(473, 346), DqnV2_2i(797, 143), DqnV2_2i(432, 88),  DqnV2_2i(38, 969),  DqnV2_2i(106, 840), DqnV2_2i(269, 277), DqnV2_2i(930, 14),
	                 DqnV2_2i(563, 188), DqnV2_2i(488, 770), DqnV2_2i(404, 215), DqnV2_2i(38, 136),  DqnV2_2i(702, 16),  DqnV2_2i(489, 18),  DqnV2_2i(832, 387),
	                 DqnV2_2i(155, 748), DqnV2_2i(224, 884), DqnV2_2i(531, 508), DqnV2_2i(480, 300), DqnV2_2i(75, 340),  DqnV2_2i(481, 779), DqnV2_2i(39, 916),
	                 DqnV2_2i(520, 125), DqnV2_2i(909, 172), DqnV2_2i(150, 665), DqnV2_2i(629, 312), DqnV2_2i(778, 24),  DqnV2_2i(129, 364), DqnV2_2i(62, 38),
	                 DqnV2_2i(228, 895), DqnV2_2i(673, 388), DqnV2_2i(81, 352),  DqnV2_2i(367, 870), DqnV2_2i(555, 31),  DqnV2_2i(420, 478), DqnV2_2i(742, 225),
	                 DqnV2_2i(192, 298), DqnV2_2i(912, 184), DqnV2_2i(372, 707), DqnV2_2i(380, 184), DqnV2_2i(376, 891), DqnV2_2i(583, 613), DqnV2_2i(709, 154),
	                 DqnV2_2i(16, 524),  DqnV2_2i(192, 866), DqnV2_2i(755, 78),  DqnV2_2i(415, 242), DqnV2_2i(294, 705), DqnV2_2i(871, 414), DqnV2_2i(469, 761),
	                 DqnV2_2i(3, 407),   DqnV2_2i(281, 292), DqnV2_2i(628, 710), DqnV2_2i(80, 598),  DqnV2_2i(440, 638), DqnV2_2i(107, 468), DqnV2_2i(682, 472),
	                 DqnV2_2i(17, 936),  DqnV2_2i(307, 270), DqnV2_2i(591, 578), DqnV2_2i(3, 192),   DqnV2_2i(684, 577), DqnV2_2i(217, 516), DqnV2_2i(823, 354),
	                 DqnV2_2i(727, 438), DqnV2_2i(330, 137), DqnV2_2i(231, 791), DqnV2_2i(941, 186), DqnV2_2i(813, 137), DqnV2_2i(796, 527), DqnV2_2i(242, 664),
	                 DqnV2_2i(880, 174), DqnV2_2i(480, 778), DqnV2_2i(587, 433), DqnV2_2i(533, 845), DqnV2_2i(506, 51),  DqnV2_2i(152, 168), DqnV2_2i(837, 111),
	                 DqnV2_2i(11, 81),   DqnV2_2i(234, 606), DqnV2_2i(673, 729), DqnV2_2i(692, 175), DqnV2_2i(394, 297), DqnV2_2i(525, 682), DqnV2_2i(490, 526),
	                 DqnV2_2i(682, 25),  DqnV2_2i(122, 395), DqnV2_2i(123, 391), DqnV2_2i(811, 179), DqnV2_2i(847, 414), DqnV2_2i(601, 36),  DqnV2_2i(78, 573),
	                 DqnV2_2i(559, 179), DqnV2_2i(599, 176), DqnV2_2i(422, 577), DqnV2_2i(855, 173), DqnV2_2i(139, 119), DqnV2_2i(366, 807), DqnV2_2i(693, 57),
	                 DqnV2_2i(193, 278), DqnV2_2i(390, 785), DqnV2_2i(967, 111), DqnV2_2i(962, 228), DqnV2_2i(227, 729), DqnV2_2i(731, 133), DqnV2_2i(28, 171),
	                 DqnV2_2i(594, 76),  DqnV2_2i(794, 233), DqnV2_2i(90, 925),  DqnV2_2i(842, 458), DqnV2_2i(164, 678), DqnV2_2i(539, 630), DqnV2_2i(889, 361),
	                 DqnV2_2i(740, 387), DqnV2_2i(415, 895), DqnV2_2i(124, 430), DqnV2_2i(624, 643), DqnV2_2i(83, 339),  DqnV2_2i(702, 102), DqnV2_2i(464, 350),
	                 DqnV2_2i(853, 480), DqnV2_2i(641, 407), DqnV2_2i(877, 134), DqnV2_2i(964, 260), DqnV2_2i(95, 653),  DqnV2_2i(474, 680), DqnV2_2i(524, 551),
	                 DqnV2_2i(384, 236), DqnV2_2i(265, 179), DqnV2_2i(777, 87),  DqnV2_2i(356, 198), DqnV2_2i(575, 109), DqnV2_2i(838, 237), DqnV2_2i(548, 680),
	                 DqnV2_2i(266, 357), DqnV2_2i(381, 594), DqnV2_2i(917, 290), DqnV2_2i(590, 11),  DqnV2_2i(158, 426), DqnV2_2i(332, 568), DqnV2_2i(66, 27),
	                 DqnV2_2i(260, 469), DqnV2_2i(865, 432), DqnV2_2i(21, 825),  DqnV2_2i(19, 716),  DqnV2_2i(90, 286),  DqnV2_2i(1, 192),   DqnV2_2i(563, 425),
	                 DqnV2_2i(344, 632), DqnV2_2i(715, 463), DqnV2_2i(594, 620), DqnV2_2i(151, 884), DqnV2_2i(86, 948),  DqnV2_2i(488, 387), DqnV2_2i(595, 218),
	                 DqnV2_2i(309, 603), DqnV2_2i(711, 177), DqnV2_2i(11, 442),  DqnV2_2i(77, 289),  DqnV2_2i(942, 50),  DqnV2_2i(114, 6),   DqnV2_2i(374, 699),
	                 DqnV2_2i(61, 35),   DqnV2_2i(42, 256),  DqnV2_2i(351, 154), DqnV2_2i(101, 485), DqnV2_2i(467, 575), DqnV2_2i(86, 217),  DqnV2_2i(53, 157),
	                 DqnV2_2i(298, 591), DqnV2_2i(689, 448), DqnV2_2i(85, 77),   DqnV2_2i(459, 552), DqnV2_2i(939, 112), DqnV2_2i(166, 760), DqnV2_2i(43, 244),
	                 DqnV2_2i(588, 23),  DqnV2_2i(27, 647),  DqnV2_2i(931, 134), DqnV2_2i(275, 767), DqnV2_2i(784, 460), DqnV2_2i(57, 115),  DqnV2_2i(151, 261),
	                 DqnV2_2i(587, 293), DqnV2_2i(213, 3),   DqnV2_2i(783, 555), DqnV2_2i(839, 24),  DqnV2_2i(70, 245),  DqnV2_2i(721, 641), DqnV2_2i(167, 260),
	                 DqnV2_2i(440, 712), DqnV2_2i(231, 518), DqnV2_2i(735, 501), DqnV2_2i(726, 210), DqnV2_2i(526, 707), DqnV2_2i(716, 425), DqnV2_2i(77, 873),
	                 DqnV2_2i(838, 192), DqnV2_2i(171, 515), DqnV2_2i(157, 86),  DqnV2_2i(124, 696), DqnV2_2i(198, 807), DqnV2_2i(324, 890), DqnV2_2i(160, 837),
	                 DqnV2_2i(346, 917), DqnV2_2i(967, 101), DqnV2_2i(834, 14),  DqnV2_2i(976, 10),  DqnV2_2i(324, 228), DqnV2_2i(397, 560), DqnV2_2i(420, 221),
	                 DqnV2_2i(353, 293), DqnV2_2i(202, 388), DqnV2_2i(151, 790), DqnV2_2i(430, 811), DqnV2_2i(294, 578), DqnV2_2i(249, 405), DqnV2_2i(62, 741),
	                 DqnV2_2i(878, 222), DqnV2_2i(572, 265), DqnV2_2i(523, 142), DqnV2_2i(684, 601), DqnV2_2i(579, 95),  DqnV2_2i(549, 694), DqnV2_2i(582, 716),
	                 DqnV2_2i(543, 561), DqnV2_2i(800, 582), DqnV2_2i(203, 348), DqnV2_2i(153, 953), DqnV2_2i(64, 38),   DqnV2_2i(17, 310),  DqnV2_2i(175, 452),
	                 DqnV2_2i(50, 740),  DqnV2_2i(501, 408), DqnV2_2i(779, 168), DqnV2_2i(284, 596), DqnV2_2i(931, 127), DqnV2_2i(149, 599), DqnV2_2i(614, 224),
	                 DqnV2_2i(827, 10),  DqnV2_2i(949, 249), DqnV2_2i(346, 442), DqnV2_2i(256, 854), DqnV2_2i(815, 121), DqnV2_2i(1, 780),   DqnV2_2i(591, 416),
	                 DqnV2_2i(134, 474), DqnV2_2i(354, 792), DqnV2_2i(4, 553),   DqnV2_2i(401, 282), DqnV2_2i(793, 154), DqnV2_2i(708, 12),  DqnV2_2i(274, 254),
	                 DqnV2_2i(375, 741), DqnV2_2i(216, 629), DqnV2_2i(673, 717), DqnV2_2i(740, 191), DqnV2_2i(546, 621), DqnV2_2i(313, 116), DqnV2_2i(568, 135),
	                 DqnV2_2i(734, 204), DqnV2_2i(798, 292), DqnV2_2i(359, 347), DqnV2_2i(224, 159), DqnV2_2i(837, 287), DqnV2_2i(730, 40),  DqnV2_2i(542, 217),
	                 DqnV2_2i(837, 101), DqnV2_2i(697, 684), DqnV2_2i(380, 384), DqnV2_2i(604, 128), DqnV2_2i(118, 626), DqnV2_2i(407, 785), DqnV2_2i(482, 200),
	                 DqnV2_2i(621, 478), DqnV2_2i(789, 138), DqnV2_2i(843, 265), DqnV2_2i(715, 539), DqnV2_2i(287, 146), DqnV2_2i(624, 396), DqnV2_2i(81, 967),
	                 DqnV2_2i(390, 135), DqnV2_2i(475, 433), DqnV2_2i(905, 80),  DqnV2_2i(254, 196), DqnV2_2i(787, 243), DqnV2_2i(404, 298), DqnV2_2i(510, 443),
	                 DqnV2_2i(305, 947), DqnV2_2i(310, 879), DqnV2_2i(543, 572), DqnV2_2i(962, 133), DqnV2_2i(185, 716), DqnV2_2i(137, 911), DqnV2_2i(430, 141),
	                 DqnV2_2i(557, 610), DqnV2_2i(106, 71),  DqnV2_2i(499, 87),  DqnV2_2i(746, 4),   DqnV2_2i(383, 329), DqnV2_2i(327, 895), DqnV2_2i(305, 840),
	                 DqnV2_2i(726, 149), DqnV2_2i(211, 782), DqnV2_2i(499, 600), DqnV2_2i(844, 182), DqnV2_2i(310, 796), DqnV2_2i(164, 983), DqnV2_2i(417, 660),
	                 DqnV2_2i(433, 537), DqnV2_2i(535, 46),  DqnV2_2i(376, 922), DqnV2_2i(295, 266), DqnV2_2i(192, 764), DqnV2_2i(266, 438), DqnV2_2i(680, 443),
	                 DqnV2_2i(66, 399),  DqnV2_2i(196, 234), DqnV2_2i(99, 573),  DqnV2_2i(773, 224), DqnV2_2i(666, 10),  DqnV2_2i(397, 383), DqnV2_2i(755, 465),
	                 DqnV2_2i(304, 451), DqnV2_2i(772, 158), DqnV2_2i(245, 480), DqnV2_2i(82, 16),   DqnV2_2i(637, 388), DqnV2_2i(824, 499), DqnV2_2i(651, 533),
	                 DqnV2_2i(725, 390), DqnV2_2i(464, 774), DqnV2_2i(167, 810), DqnV2_2i(315, 92),  DqnV2_2i(722, 494), DqnV2_2i(855, 350), DqnV2_2i(949, 152),
	                 DqnV2_2i(446, 504), DqnV2_2i(299, 275), DqnV2_2i(570, 658), DqnV2_2i(408, 828), DqnV2_2i(877, 292), DqnV2_2i(340, 231), DqnV2_2i(643, 389),
	                 DqnV2_2i(263, 358), DqnV2_2i(217, 528), DqnV2_2i(1, 818),   DqnV2_2i(950, 250), DqnV2_2i(146, 974), DqnV2_2i(199, 91),  DqnV2_2i(117, 416),
	                 DqnV2_2i(417, 879), DqnV2_2i(151, 764), DqnV2_2i(263, 728), DqnV2_2i(758, 606), DqnV2_2i(364, 458), DqnV2_2i(708, 308), DqnV2_2i(842, 387),
	                 DqnV2_2i(147, 718), DqnV2_2i(81, 908),  DqnV2_2i(50, 571),  DqnV2_2i(443, 216), DqnV2_2i(485, 753), DqnV2_2i(60, 856),  DqnV2_2i(924, 320),
	                 DqnV2_2i(502, 752), DqnV2_2i(34, 252),  DqnV2_2i(515, 282), DqnV2_2i(692, 389), DqnV2_2i(5, 758),   DqnV2_2i(526, 185), DqnV2_2i(77, 910),
	                 DqnV2_2i(516, 26),  DqnV2_2i(791, 446), DqnV2_2i(132, 703), DqnV2_2i(235, 102), DqnV2_2i(329, 270), DqnV2_2i(493, 848), DqnV2_2i(29, 79),
	                 DqnV2_2i(367, 594), DqnV2_2i(65, 25),   DqnV2_2i(308, 387), DqnV2_2i(672, 480), DqnV2_2i(384, 487), DqnV2_2i(629, 251), DqnV2_2i(787, 561),
	                 DqnV2_2i(393, 856), DqnV2_2i(597, 390), DqnV2_2i(279, 318), DqnV2_2i(849, 80),  DqnV2_2i(154, 40),  DqnV2_2i(886, 422), DqnV2_2i(177, 882),
	                 DqnV2_2i(507, 83),  DqnV2_2i(304, 404), DqnV2_2i(920, 204), DqnV2_2i(826, 177), DqnV2_2i(465, 383), DqnV2_2i(388, 680), DqnV2_2i(381, 468),
	                 DqnV2_2i(449, 619), DqnV2_2i(254, 878), DqnV2_2i(552, 807), DqnV2_2i(683, 367), DqnV2_2i(913, 146), DqnV2_2i(26, 351),  DqnV2_2i(434, 687),
	                 DqnV2_2i(440, 799), DqnV2_2i(64, 653),  DqnV2_2i(122, 604), DqnV2_2i(166, 820), DqnV2_2i(469, 437), DqnV2_2i(704, 3),   DqnV2_2i(630, 180),
	                 DqnV2_2i(520, 465), DqnV2_2i(901, 401), DqnV2_2i(74, 740),  DqnV2_2i(100, 265), DqnV2_2i(357, 223), DqnV2_2i(529, 790), DqnV2_2i(581, 11),
	                 DqnV2_2i(703, 542), DqnV2_2i(110, 216), DqnV2_2i(597, 51),  DqnV2_2i(221, 356), DqnV2_2i(305, 795), DqnV2_2i(102, 983), DqnV2_2i(9, 917),
	                 DqnV2_2i(844, 43),  DqnV2_2i(877, 452), DqnV2_2i(641, 28),  DqnV2_2i(626, 770), DqnV2_2i(860, 37),  DqnV2_2i(171, 766), DqnV2_2i(93, 168),
	                 DqnV2_2i(15, 27),   DqnV2_2i(472, 613), DqnV2_2i(224, 680), DqnV2_2i(1, 939),   DqnV2_2i(790, 432), DqnV2_2i(899, 179), DqnV2_2i(573, 243),
	                 DqnV2_2i(842, 397), DqnV2_2i(155, 72),  DqnV2_2i(344, 811), DqnV2_2i(799, 111), DqnV2_2i(296, 706), DqnV2_2i(283, 836), DqnV2_2i(386, 296),
	                 DqnV2_2i(429, 362), DqnV2_2i(627, 44),  DqnV2_2i(16, 271),  DqnV2_2i(812, 148), DqnV2_2i(2, 953),   DqnV2_2i(904, 385), DqnV2_2i(135, 639),
	                 DqnV2_2i(886, 454), DqnV2_2i(4, 940),   DqnV2_2i(552, 329), DqnV2_2i(555, 750), DqnV2_2i(102, 924), DqnV2_2i(122, 672), DqnV2_2i(122, 667),
	                 DqnV2_2i(318, 843), DqnV2_2i(126, 63),  DqnV2_2i(162, 390), DqnV2_2i(137, 972), DqnV2_2i(531, 178), DqnV2_2i(46, 889),  DqnV2_2i(312, 72),
	                 DqnV2_2i(445, 891), DqnV2_2i(123, 359), DqnV2_2i(184, 791), DqnV2_2i(292, 498), DqnV2_2i(674, 39),  DqnV2_2i(3, 155),   DqnV2_2i(23, 188),
	                 DqnV2_2i(671, 71),  DqnV2_2i(532, 510), DqnV2_2i(430, 349), DqnV2_2i(221, 818), DqnV2_2i(623, 364), DqnV2_2i(969, 63),  DqnV2_2i(164, 708),
	                 DqnV2_2i(325, 205), DqnV2_2i(129, 343), DqnV2_2i(785, 40),  DqnV2_2i(793, 511), DqnV2_2i(701, 187), DqnV2_2i(402, 437), DqnV2_2i(176, 122),
	                 DqnV2_2i(198, 614), DqnV2_2i(204, 876), DqnV2_2i(828, 396), DqnV2_2i(88, 168),  DqnV2_2i(579, 804), DqnV2_2i(790, 305), DqnV2_2i(721, 347),
	                 DqnV2_2i(291, 195), DqnV2_2i(318, 868), DqnV2_2i(757, 651), DqnV2_2i(729, 177), DqnV2_2i(400, 488), DqnV2_2i(229, 442), DqnV2_2i(567, 405),
	                 DqnV2_2i(406, 18),  DqnV2_2i(525, 768), DqnV2_2i(347, 697), DqnV2_2i(271, 95),  DqnV2_2i(589, 182), DqnV2_2i(73, 394),  DqnV2_2i(142, 597),
	                 DqnV2_2i(621, 384), DqnV2_2i(715, 530), DqnV2_2i(548, 236), DqnV2_2i(135, 224), DqnV2_2i(444, 348), DqnV2_2i(274, 671), DqnV2_2i(262, 917),
	                 DqnV2_2i(177, 809), DqnV2_2i(691, 549), DqnV2_2i(368, 852), DqnV2_2i(263, 177), DqnV2_2i(809, 454), DqnV2_2i(473, 864), DqnV2_2i(248, 704),
	                 DqnV2_2i(195, 942), DqnV2_2i(256, 20),  DqnV2_2i(278, 395), DqnV2_2i(28, 789),  DqnV2_2i(142, 229), DqnV2_2i(463, 452), DqnV2_2i(330, 168),
	                 DqnV2_2i(296, 271), DqnV2_2i(495, 387), DqnV2_2i(830, 516), DqnV2_2i(407, 70),  DqnV2_2i(650, 118), DqnV2_2i(115, 100), DqnV2_2i(885, 353),
	                 DqnV2_2i(266, 541), DqnV2_2i(470, 158), DqnV2_2i(480, 397), DqnV2_2i(81, 835),  DqnV2_2i(715, 372), DqnV2_2i(749, 383), DqnV2_2i(684, 685),
	                 DqnV2_2i(104, 803), DqnV2_2i(596, 710), DqnV2_2i(316, 361), DqnV2_2i(110, 923), DqnV2_2i(116, 940), DqnV2_2i(54, 562),  DqnV2_2i(604, 589),
	                 DqnV2_2i(284, 672), DqnV2_2i(457, 440), DqnV2_2i(454, 832), DqnV2_2i(6, 225),   DqnV2_2i(61, 852),  DqnV2_2i(724, 75),  DqnV2_2i(54, 695),
	                 DqnV2_2i(348, 910), DqnV2_2i(569, 86),  DqnV2_2i(536, 35),  DqnV2_2i(611, 493), DqnV2_2i(435, 480), DqnV2_2i(635, 494), DqnV2_2i(625, 534),
	                 DqnV2_2i(668, 194), DqnV2_2i(567, 610), DqnV2_2i(121, 354), DqnV2_2i(865, 156), DqnV2_2i(462, 193), DqnV2_2i(152, 401), DqnV2_2i(100, 323),
	                 DqnV2_2i(43, 667),  DqnV2_2i(571, 365), DqnV2_2i(208, 796), DqnV2_2i(603, 332), DqnV2_2i(90, 666),  DqnV2_2i(900, 373), DqnV2_2i(223, 294),
	                 DqnV2_2i(306, 330), DqnV2_2i(484, 333), DqnV2_2i(327, 237), DqnV2_2i(338, 433), DqnV2_2i(388, 112), DqnV2_2i(892, 192), DqnV2_2i(975, 64),
	                 DqnV2_2i(953, 266), DqnV2_2i(890, 130), DqnV2_2i(516, 680), DqnV2_2i(206, 251), DqnV2_2i(656, 640), DqnV2_2i(8, 440),   DqnV2_2i(234, 409),
	                 DqnV2_2i(152, 14),  DqnV2_2i(556, 475), DqnV2_2i(627, 721), DqnV2_2i(427, 60),  DqnV2_2i(506, 373), DqnV2_2i(222, 542), DqnV2_2i(747, 483),
	                 DqnV2_2i(733, 484), DqnV2_2i(367, 259), DqnV2_2i(501, 143), DqnV2_2i(221, 171), DqnV2_2i(931, 293), DqnV2_2i(474, 498), DqnV2_2i(357, 833),
	                 DqnV2_2i(1, 319),   DqnV2_2i(187, 828), DqnV2_2i(134, 226), DqnV2_2i(108, 382), DqnV2_2i(228, 725), DqnV2_2i(437, 179), DqnV2_2i(827, 235),
	                 DqnV2_2i(689, 130), DqnV2_2i(943, 274), DqnV2_2i(656, 385), DqnV2_2i(771, 253), DqnV2_2i(10, 803),  DqnV2_2i(236, 184), DqnV2_2i(699, 217),
	                 DqnV2_2i(644, 699), DqnV2_2i(238, 892), DqnV2_2i(625, 237), DqnV2_2i(202, 623), DqnV2_2i(189, 79),  DqnV2_2i(552, 403), DqnV2_2i(669, 521),
	                 DqnV2_2i(250, 45),  DqnV2_2i(343, 58),  DqnV2_2i(264, 249), DqnV2_2i(625, 666), DqnV2_2i(194, 555), DqnV2_2i(278, 385), DqnV2_2i(527, 722),
	                 DqnV2_2i(967, 26),  DqnV2_2i(875, 479), DqnV2_2i(594, 575), DqnV2_2i(437, 565), DqnV2_2i(10, 338),  DqnV2_2i(15, 904),  DqnV2_2i(863, 240),
	                 DqnV2_2i(701, 676), DqnV2_2i(949, 222), DqnV2_2i(745, 403), DqnV2_2i(185, 370), DqnV2_2i(266, 216), DqnV2_2i(269, 306), DqnV2_2i(162, 198),
	                 DqnV2_2i(514, 124), DqnV2_2i(411, 157), DqnV2_2i(636, 375), DqnV2_2i(528, 506), DqnV2_2i(698, 431), DqnV2_2i(749, 488), DqnV2_2i(49, 303),
	                 DqnV2_2i(796, 580), DqnV2_2i(159, 465), DqnV2_2i(921, 149), DqnV2_2i(453, 752), DqnV2_2i(133, 517), DqnV2_2i(259, 925), DqnV2_2i(187, 947),
	                 DqnV2_2i(743, 335), DqnV2_2i(64, 255),  DqnV2_2i(449, 193), DqnV2_2i(654, 684), DqnV2_2i(561, 471), DqnV2_2i(648, 109), DqnV2_2i(897, 28),
	                 DqnV2_2i(661, 271), DqnV2_2i(604, 770), DqnV2_2i(820, 268), DqnV2_2i(457, 210), DqnV2_2i(636, 610), DqnV2_2i(662, 599), DqnV2_2i(88, 206),
	                 DqnV2_2i(995, 2),   DqnV2_2i(581, 344), DqnV2_2i(471, 294), DqnV2_2i(28, 612),  DqnV2_2i(696, 130), DqnV2_2i(850, 421), DqnV2_2i(673, 192),
	                 DqnV2_2i(28, 759),  DqnV2_2i(147, 570), DqnV2_2i(888, 257), DqnV2_2i(854, 316), DqnV2_2i(167, 854), DqnV2_2i(137, 121), DqnV2_2i(909, 13),
	                 DqnV2_2i(797, 94),  DqnV2_2i(255, 324), DqnV2_2i(512, 585), DqnV2_2i(149, 798), DqnV2_2i(536, 305), DqnV2_2i(192, 560), DqnV2_2i(460, 271),
	                 DqnV2_2i(647, 320), DqnV2_2i(779, 538), DqnV2_2i(92, 645),  DqnV2_2i(152, 640), DqnV2_2i(769, 483), DqnV2_2i(882, 83),  DqnV2_2i(518, 365),
	                 DqnV2_2i(83, 304),  DqnV2_2i(172, 956), DqnV2_2i(617, 79),  DqnV2_2i(538, 682), DqnV2_2i(353, 443), DqnV2_2i(584, 171), DqnV2_2i(432, 850),
	                 DqnV2_2i(424, 348), DqnV2_2i(29, 140),  DqnV2_2i(635, 126), DqnV2_2i(49, 524),  DqnV2_2i(208, 783), DqnV2_2i(337, 234), DqnV2_2i(194, 929),
	                 DqnV2_2i(35, 128),  DqnV2_2i(97, 947),  DqnV2_2i(332, 872), DqnV2_2i(243, 860), DqnV2_2i(163, 909), DqnV2_2i(385, 819), DqnV2_2i(328, 258),
	                 DqnV2_2i(677, 250), DqnV2_2i(364, 155), DqnV2_2i(523, 108), DqnV2_2i(116, 588), DqnV2_2i(718, 634), DqnV2_2i(567, 430), DqnV2_2i(636, 233),
	                 DqnV2_2i(631, 538), DqnV2_2i(407, 761), DqnV2_2i(444, 142), DqnV2_2i(509, 219), DqnV2_2i(419, 298), DqnV2_2i(853, 90),  DqnV2_2i(196, 716),
	                 DqnV2_2i(399, 378), DqnV2_2i(913, 394), DqnV2_2i(861, 41),  DqnV2_2i(490, 18),  DqnV2_2i(842, 515), DqnV2_2i(509, 662), DqnV2_2i(669, 389),
	                 DqnV2_2i(89, 583),  DqnV2_2i(16, 892),  DqnV2_2i(902, 296), DqnV2_2i(22, 684),  DqnV2_2i(532, 178), DqnV2_2i(703, 564), DqnV2_2i(655, 570),
	                 DqnV2_2i(298, 219), DqnV2_2i(379, 791), DqnV2_2i(444, 194), DqnV2_2i(399, 510), DqnV2_2i(509, 609), DqnV2_2i(715, 321), DqnV2_2i(28, 940),
	                 DqnV2_2i(604, 166), DqnV2_2i(71, 378),  DqnV2_2i(217, 6),   DqnV2_2i(740, 574), DqnV2_2i(401, 298), DqnV2_2i(33, 520),  DqnV2_2i(940, 255),
	                 DqnV2_2i(251, 370), DqnV2_2i(873, 286), DqnV2_2i(876, 107), DqnV2_2i(338, 862), DqnV2_2i(547, 9),   DqnV2_2i(320, 678), DqnV2_2i(181, 849),
	                 DqnV2_2i(418, 862), DqnV2_2i(145, 75),  DqnV2_2i(202, 795), DqnV2_2i(193, 107), DqnV2_2i(7, 594),   DqnV2_2i(189, 28),  DqnV2_2i(130, 585),
	                 DqnV2_2i(813, 220), DqnV2_2i(328, 456), DqnV2_2i(118, 847), DqnV2_2i(520, 33),  DqnV2_2i(472, 355), DqnV2_2i(44, 216),  DqnV2_2i(359, 907),
	                 DqnV2_2i(107, 678), DqnV2_2i(180, 400), DqnV2_2i(37, 485),  DqnV2_2i(37, 602),  DqnV2_2i(656, 447), DqnV2_2i(348, 517), DqnV2_2i(450, 214),
	                 DqnV2_2i(414, 254), DqnV2_2i(73, 335),  DqnV2_2i(494, 652), DqnV2_2i(242, 285), DqnV2_2i(528, 388), DqnV2_2i(85, 256),  DqnV2_2i(3, 573),
	                 DqnV2_2i(46, 455),  DqnV2_2i(957, 98),  DqnV2_2i(218, 314), DqnV2_2i(956, 23),  DqnV2_2i(950, 238), DqnV2_2i(505, 94),  DqnV2_2i(83, 863),
	                 DqnV2_2i(10, 654),  DqnV2_2i(417, 46),  DqnV2_2i(891, 145), DqnV2_2i(173, 245), DqnV2_2i(663, 75),  DqnV2_2i(44, 479),  DqnV2_2i(181, 656),
	                 DqnV2_2i(468, 881), DqnV2_2i(758, 135), DqnV2_2i(448, 301), DqnV2_2i(417, 393), DqnV2_2i(201, 176), DqnV2_2i(452, 263), DqnV2_2i(135, 565),
	                 DqnV2_2i(870, 415), DqnV2_2i(131, 101), DqnV2_2i(473, 31),  DqnV2_2i(734, 283), DqnV2_2i(30, 849),  DqnV2_2i(929, 4),   DqnV2_2i(778, 113),
	                 DqnV2_2i(688, 571), DqnV2_2i(638, 714), DqnV2_2i(195, 697), DqnV2_2i(302, 260), DqnV2_2i(755, 627), DqnV2_2i(400, 278), DqnV2_2i(707, 487),
	                 DqnV2_2i(166, 388), DqnV2_2i(991, 109), DqnV2_2i(634, 730), DqnV2_2i(782, 605), DqnV2_2i(644, 611), DqnV2_2i(19, 199),  DqnV2_2i(112, 15),
	                 DqnV2_2i(883, 302), DqnV2_2i(642, 96),  DqnV2_2i(204, 294), DqnV2_2i(809, 512), DqnV2_2i(68, 73),   DqnV2_2i(157, 110), DqnV2_2i(231, 586),
	                 DqnV2_2i(393, 615), DqnV2_2i(174, 128), DqnV2_2i(30, 553),  DqnV2_2i(355, 488), DqnV2_2i(147, 341), DqnV2_2i(456, 792), DqnV2_2i(51, 332),
	                 DqnV2_2i(25, 42),   DqnV2_2i(173, 350), DqnV2_2i(533, 129), DqnV2_2i(567, 604), DqnV2_2i(305, 823), DqnV2_2i(29, 15),   DqnV2_2i(700, 662),
	                 DqnV2_2i(52, 920),  DqnV2_2i(7, 64),    DqnV2_2i(981, 106), DqnV2_2i(557, 713), DqnV2_2i(73, 627),  DqnV2_2i(923, 336), DqnV2_2i(78, 471),
	                 DqnV2_2i(855, 83),  DqnV2_2i(453, 442), DqnV2_2i(182, 288), DqnV2_2i(820, 423), DqnV2_2i(151, 232), DqnV2_2i(650, 694), DqnV2_2i(252, 483),
	                 DqnV2_2i(699, 358), DqnV2_2i(459, 699), DqnV2_2i(686, 146), DqnV2_2i(329, 673), DqnV2_2i(781, 579), DqnV2_2i(62, 79),   DqnV2_2i(47, 702),
	                 DqnV2_2i(105, 687), DqnV2_2i(182, 140), DqnV2_2i(566, 823), DqnV2_2i(279, 245), DqnV2_2i(36, 405),  DqnV2_2i(556, 220), DqnV2_2i(381, 615),
	                 DqnV2_2i(65, 424),  DqnV2_2i(263, 561), DqnV2_2i(99, 283),  DqnV2_2i(7, 360),   DqnV2_2i(727, 283), DqnV2_2i(49, 729),  DqnV2_2i(760, 201),
	                 DqnV2_2i(134, 101), DqnV2_2i(447, 665), DqnV2_2i(607, 312), DqnV2_2i(581, 140), DqnV2_2i(690, 97),  DqnV2_2i(677, 52),  DqnV2_2i(199, 485),
	                 DqnV2_2i(461, 841), DqnV2_2i(715, 288), DqnV2_2i(92, 688),  DqnV2_2i(482, 443), DqnV2_2i(863, 257), DqnV2_2i(74, 414),  DqnV2_2i(651, 371),
	                 DqnV2_2i(21, 723),  DqnV2_2i(840, 509), DqnV2_2i(373, 347), DqnV2_2i(891, 434), DqnV2_2i(797, 121), DqnV2_2i(274, 563), DqnV2_2i(617, 146),
	                 DqnV2_2i(545, 365), DqnV2_2i(163, 571), DqnV2_2i(675, 108), DqnV2_2i(206, 94),  DqnV2_2i(466, 309), DqnV2_2i(87, 646),  DqnV2_2i(202, 627),
	                 DqnV2_2i(432, 63),  DqnV2_2i(798, 381), DqnV2_2i(161, 146), DqnV2_2i(280, 751), DqnV2_2i(32, 599),  DqnV2_2i(320, 460), DqnV2_2i(206, 34),
	                 DqnV2_2i(744, 640), DqnV2_2i(607, 752), DqnV2_2i(102, 829), DqnV2_2i(374, 55),  DqnV2_2i(745, 493), DqnV2_2i(514, 323), DqnV2_2i(820, 137),
	                 DqnV2_2i(437, 38),  DqnV2_2i(834, 71),  DqnV2_2i(716, 375), DqnV2_2i(759, 134), DqnV2_2i(123, 870), DqnV2_2i(260, 57),  DqnV2_2i(809, 10),
	                 DqnV2_2i(797, 101), DqnV2_2i(10, 620),  DqnV2_2i(297, 863), DqnV2_2i(462, 221), DqnV2_2i(370, 336), DqnV2_2i(335, 79),  DqnV2_2i(194, 384),
	                 DqnV2_2i(831, 87),  DqnV2_2i(741, 70),  DqnV2_2i(511, 76),  DqnV2_2i(602, 92),  DqnV2_2i(900, 208), DqnV2_2i(270, 71),  DqnV2_2i(272, 132),
	                 DqnV2_2i(487, 108), DqnV2_2i(236, 601), DqnV2_2i(271, 513), DqnV2_2i(737, 33),  DqnV2_2i(549, 278), DqnV2_2i(43, 121),  DqnV2_2i(673, 49),
	                 DqnV2_2i(119, 164), DqnV2_2i(376, 842), DqnV2_2i(519, 727), DqnV2_2i(594, 571), DqnV2_2i(467, 474), DqnV2_2i(860, 431), DqnV2_2i(412, 114),
	                 DqnV2_2i(261, 30),  DqnV2_2i(243, 660), DqnV2_2i(57, 335),  DqnV2_2i(532, 554), DqnV2_2i(296, 321), DqnV2_2i(279, 520), DqnV2_2i(196, 75),
	                 DqnV2_2i(407, 428), DqnV2_2i(426, 446), DqnV2_2i(174, 308), DqnV2_2i(151, 887), DqnV2_2i(44, 371),  DqnV2_2i(294, 952), DqnV2_2i(233, 761),
	                 DqnV2_2i(686, 693), DqnV2_2i(709, 590), DqnV2_2i(55, 69),   DqnV2_2i(165, 266), DqnV2_2i(124, 657), DqnV2_2i(224, 782), DqnV2_2i(192, 771),
	                 DqnV2_2i(986, 120), DqnV2_2i(825, 77),  DqnV2_2i(358, 763), DqnV2_2i(446, 228), DqnV2_2i(11, 542),  DqnV2_2i(95, 435),  DqnV2_2i(812, 150),
	                 DqnV2_2i(330, 624), DqnV2_2i(290, 721), DqnV2_2i(586, 520), DqnV2_2i(301, 646), DqnV2_2i(257, 747), DqnV2_2i(266, 126), DqnV2_2i(284, 869),
	                 DqnV2_2i(360, 707), DqnV2_2i(337, 924), DqnV2_2i(513, 323), DqnV2_2i(365, 819), DqnV2_2i(789, 244), DqnV2_2i(677, 618), DqnV2_2i(651, 395),
	                 DqnV2_2i(866, 104), DqnV2_2i(231, 921), DqnV2_2i(14, 242),  DqnV2_2i(455, 443), DqnV2_2i(934, 168), DqnV2_2i(3, 695),   DqnV2_2i(323, 390),
	                 DqnV2_2i(479, 845), DqnV2_2i(418, 444), DqnV2_2i(802, 338), DqnV2_2i(530, 434), DqnV2_2i(222, 221), DqnV2_2i(665, 290), DqnV2_2i(295, 190),
	                 DqnV2_2i(817, 108), DqnV2_2i(782, 595), DqnV2_2i(366, 656), DqnV2_2i(606, 188), DqnV2_2i(712, 695), DqnV2_2i(236, 629), DqnV2_2i(80, 891),
	                 DqnV2_2i(719, 488), DqnV2_2i(318, 575), DqnV2_2i(147, 262), DqnV2_2i(217, 266), DqnV2_2i(298, 218), DqnV2_2i(134, 471), DqnV2_2i(675, 686),
	                 DqnV2_2i(667, 399), DqnV2_2i(588, 96),  DqnV2_2i(693, 393), DqnV2_2i(861, 112), DqnV2_2i(5, 885),   DqnV2_2i(679, 451), DqnV2_2i(345, 297),
	                 DqnV2_2i(394, 401), DqnV2_2i(117, 32),  DqnV2_2i(93, 428),  DqnV2_2i(487, 410), DqnV2_2i(539, 325), DqnV2_2i(883, 31),  DqnV2_2i(15, 797),
	                 DqnV2_2i(293, 688), DqnV2_2i(753, 377), DqnV2_2i(723, 128), DqnV2_2i(696, 381), DqnV2_2i(352, 528), DqnV2_2i(310, 548), DqnV2_2i(355, 696),
	                 DqnV2_2i(611, 16),  DqnV2_2i(146, 526), DqnV2_2i(816, 52),  DqnV2_2i(161, 181), DqnV2_2i(354, 678), DqnV2_2i(576, 286), DqnV2_2i(628, 559),
	                 DqnV2_2i(437, 604), DqnV2_2i(371, 519), DqnV2_2i(279, 127), DqnV2_2i(961, 215), DqnV2_2i(182, 426), DqnV2_2i(489, 274), DqnV2_2i(744, 217),
	                 DqnV2_2i(485, 355), DqnV2_2i(318, 133), DqnV2_2i(168, 879), DqnV2_2i(213, 886), DqnV2_2i(716, 59),  DqnV2_2i(88, 19),   DqnV2_2i(101, 760),
	                 DqnV2_2i(409, 677), DqnV2_2i(825, 373), DqnV2_2i(880, 448), DqnV2_2i(719, 688), DqnV2_2i(340, 457), DqnV2_2i(350, 462), DqnV2_2i(577, 516),
	                 DqnV2_2i(466, 48),  DqnV2_2i(653, 670), DqnV2_2i(944, 311), DqnV2_2i(384, 818), DqnV2_2i(87, 608),  DqnV2_2i(977, 212), DqnV2_2i(232, 836),
	                 DqnV2_2i(200, 798), DqnV2_2i(86, 292),  DqnV2_2i(839, 313), DqnV2_2i(173, 844), DqnV2_2i(709, 40),  DqnV2_2i(128, 736), DqnV2_2i(250, 836),
	                 DqnV2_2i(461, 440), DqnV2_2i(787, 393), DqnV2_2i(557, 516), DqnV2_2i(393, 294), DqnV2_2i(31, 534),  DqnV2_2i(452, 2),   DqnV2_2i(646, 114),
	                 DqnV2_2i(103, 34),  DqnV2_2i(786, 347), DqnV2_2i(459, 516), DqnV2_2i(42, 527),  DqnV2_2i(306, 717), DqnV2_2i(211, 750), DqnV2_2i(596, 335),
	                 DqnV2_2i(398, 233), DqnV2_2i(28, 411),  DqnV2_2i(251, 302), DqnV2_2i(408, 165), DqnV2_2i(53, 481),  DqnV2_2i(273, 57),  DqnV2_2i(306, 812),
	                 DqnV2_2i(809, 272), DqnV2_2i(96, 241),  DqnV2_2i(244, 940), DqnV2_2i(88, 422),  DqnV2_2i(21, 224),  DqnV2_2i(136, 591), DqnV2_2i(469, 611),
	                 DqnV2_2i(415, 403), DqnV2_2i(782, 578), DqnV2_2i(825, 210), DqnV2_2i(897, 213), DqnV2_2i(316, 643), DqnV2_2i(293, 353), DqnV2_2i(313, 752),
	                 DqnV2_2i(501, 775), DqnV2_2i(302, 638), DqnV2_2i(664, 8),   DqnV2_2i(414, 798), DqnV2_2i(767, 190), DqnV2_2i(35, 590),  DqnV2_2i(173, 341),
	                 DqnV2_2i(428, 867), DqnV2_2i(383, 301), DqnV2_2i(377, 526), DqnV2_2i(604, 794), DqnV2_2i(946, 257), DqnV2_2i(695, 386), DqnV2_2i(465, 343),
	                 DqnV2_2i(874, 182), DqnV2_2i(909, 349), DqnV2_2i(171, 539), DqnV2_2i(500, 220), DqnV2_2i(281, 377), DqnV2_2i(565, 567), DqnV2_2i(334, 612),
	                 DqnV2_2i(99, 640),  DqnV2_2i(464, 283), DqnV2_2i(957, 150), DqnV2_2i(301, 14),  DqnV2_2i(598, 747), DqnV2_2i(685, 141), DqnV2_2i(70, 122),
	                 DqnV2_2i(567, 797), DqnV2_2i(790, 20),  DqnV2_2i(212, 910), DqnV2_2i(150, 538), DqnV2_2i(703, 531), DqnV2_2i(733, 466), DqnV2_2i(989, 106),
	                 DqnV2_2i(649, 551), DqnV2_2i(10, 505),  DqnV2_2i(80, 162),  DqnV2_2i(190, 877), DqnV2_2i(748, 48),  DqnV2_2i(279, 201), DqnV2_2i(381, 495),
	                 DqnV2_2i(31, 86),   DqnV2_2i(385, 115), DqnV2_2i(22, 503),  DqnV2_2i(155, 296), DqnV2_2i(85, 984),  DqnV2_2i(756, 15),  DqnV2_2i(906, 218),
	                 DqnV2_2i(364, 310), DqnV2_2i(329, 778), DqnV2_2i(828, 76),  DqnV2_2i(416, 295), DqnV2_2i(841, 522), DqnV2_2i(196, 24),  DqnV2_2i(135, 737),
	                 DqnV2_2i(97, 454),  DqnV2_2i(961, 95),  DqnV2_2i(901, 359), DqnV2_2i(70, 822),  DqnV2_2i(168, 237), DqnV2_2i(397, 135), DqnV2_2i(308, 476),
	                 DqnV2_2i(648, 136), DqnV2_2i(901, 142), DqnV2_2i(595, 308), DqnV2_2i(507, 57),  DqnV2_2i(569, 509), DqnV2_2i(844, 157), DqnV2_2i(222, 371),
	                 DqnV2_2i(500, 387), DqnV2_2i(620, 154), DqnV2_2i(519, 339), DqnV2_2i(570, 612), DqnV2_2i(438, 359), DqnV2_2i(725, 597), DqnV2_2i(365, 128),
	                 DqnV2_2i(156, 4),   DqnV2_2i(781, 24),  DqnV2_2i(619, 464), DqnV2_2i(841, 14),  DqnV2_2i(199, 184), DqnV2_2i(609, 102), DqnV2_2i(92, 473),
	                 DqnV2_2i(789, 163), DqnV2_2i(30, 601),  DqnV2_2i(2, 284),   DqnV2_2i(191, 393), DqnV2_2i(141, 882), DqnV2_2i(180, 464), DqnV2_2i(465, 169),
	                 DqnV2_2i(265, 508), DqnV2_2i(300, 537), DqnV2_2i(440, 661), DqnV2_2i(158, 895), DqnV2_2i(102, 216), DqnV2_2i(74, 230),  DqnV2_2i(732, 12),
	                 DqnV2_2i(574, 623), DqnV2_2i(120, 94),  DqnV2_2i(483, 410), DqnV2_2i(839, 379), DqnV2_2i(169, 39),  DqnV2_2i(539, 148), DqnV2_2i(122, 53),
	                 DqnV2_2i(41, 440),  DqnV2_2i(284, 457), DqnV2_2i(239, 375), DqnV2_2i(787, 419), DqnV2_2i(380, 851), DqnV2_2i(779, 123), DqnV2_2i(813, 83),
	                 DqnV2_2i(130, 441), DqnV2_2i(169, 71),  DqnV2_2i(98, 189),  DqnV2_2i(912, 158), DqnV2_2i(521, 166), DqnV2_2i(462, 482), DqnV2_2i(298, 756),
	                 DqnV2_2i(222, 477), DqnV2_2i(108, 616), DqnV2_2i(406, 624), DqnV2_2i(381, 147), DqnV2_2i(590, 165), DqnV2_2i(229, 784), DqnV2_2i(442, 434),
	                 DqnV2_2i(868, 406), DqnV2_2i(26, 193),  DqnV2_2i(273, 267), DqnV2_2i(128, 420), DqnV2_2i(235, 226), DqnV2_2i(300, 798), DqnV2_2i(297, 65),
	                 DqnV2_2i(793, 563), DqnV2_2i(82, 919),  DqnV2_2i(304, 747), DqnV2_2i(837, 459), DqnV2_2i(94, 65),   DqnV2_2i(120, 86),  DqnV2_2i(676, 556),
	                 DqnV2_2i(165, 40),  DqnV2_2i(906, 323), DqnV2_2i(721, 663), DqnV2_2i(372, 739), DqnV2_2i(860, 433), DqnV2_2i(833, 367), DqnV2_2i(540, 344),
	                 DqnV2_2i(431, 756), DqnV2_2i(687, 405), DqnV2_2i(317, 215), DqnV2_2i(775, 608), DqnV2_2i(289, 799), DqnV2_2i(465, 629), DqnV2_2i(404, 134),
	                 DqnV2_2i(884, 260), DqnV2_2i(640, 629), DqnV2_2i(127, 31),  DqnV2_2i(835, 185), DqnV2_2i(43, 536),  DqnV2_2i(310, 843), DqnV2_2i(45, 506),
	                 DqnV2_2i(791, 310), DqnV2_2i(754, 271), DqnV2_2i(356, 375), DqnV2_2i(160, 786), DqnV2_2i(91, 146),  DqnV2_2i(267, 352), DqnV2_2i(142, 113),
	                 DqnV2_2i(21, 565),  DqnV2_2i(306, 669), DqnV2_2i(144, 782), DqnV2_2i(595, 372), DqnV2_2i(962, 151), DqnV2_2i(818, 283), DqnV2_2i(260, 777),
	                 DqnV2_2i(618, 377), DqnV2_2i(563, 547), DqnV2_2i(448, 178), DqnV2_2i(400, 666), DqnV2_2i(713, 76),  DqnV2_2i(580, 212), DqnV2_2i(299, 719),
	                 DqnV2_2i(432, 593), DqnV2_2i(483, 146), DqnV2_2i(501, 461), DqnV2_2i(62, 950),  DqnV2_2i(463, 733), DqnV2_2i(821, 482), DqnV2_2i(114, 864),
	                 DqnV2_2i(54, 734),  DqnV2_2i(30, 153),  DqnV2_2i(903, 147), DqnV2_2i(335, 918), DqnV2_2i(308, 822), DqnV2_2i(775, 84),  DqnV2_2i(260, 76),
	                 DqnV2_2i(520, 511), DqnV2_2i(121, 548), DqnV2_2i(112, 26),  DqnV2_2i(770, 549), DqnV2_2i(228, 456), DqnV2_2i(366, 864), DqnV2_2i(92, 622),
	                 DqnV2_2i(458, 132), DqnV2_2i(624, 602), DqnV2_2i(787, 185), DqnV2_2i(898, 378), DqnV2_2i(300, 178), DqnV2_2i(230, 555), DqnV2_2i(249, 216),
	                 DqnV2_2i(305, 71),  DqnV2_2i(502, 663), DqnV2_2i(288, 257), DqnV2_2i(563, 17),  DqnV2_2i(431, 36),  DqnV2_2i(31, 729),  DqnV2_2i(131, 344),
	                 DqnV2_2i(311, 466), DqnV2_2i(480, 497), DqnV2_2i(332, 897), DqnV2_2i(872, 194), DqnV2_2i(388, 783), DqnV2_2i(306, 755), DqnV2_2i(539, 318),
	                 DqnV2_2i(732, 205), DqnV2_2i(392, 262), DqnV2_2i(657, 97),  DqnV2_2i(4, 34),    DqnV2_2i(212, 43),  DqnV2_2i(445, 65),  DqnV2_2i(360, 368),
	                 DqnV2_2i(179, 736), DqnV2_2i(470, 191), DqnV2_2i(612, 561), DqnV2_2i(829, 145), DqnV2_2i(308, 433), DqnV2_2i(620, 568), DqnV2_2i(226, 407),
	                 DqnV2_2i(627, 513), DqnV2_2i(130, 71),  DqnV2_2i(89, 822),  DqnV2_2i(713, 655), DqnV2_2i(7, 424),   DqnV2_2i(915, 371), DqnV2_2i(38, 762),
	                 DqnV2_2i(506, 852), DqnV2_2i(357, 104), DqnV2_2i(351, 277), DqnV2_2i(144, 460), DqnV2_2i(77, 617),  DqnV2_2i(42, 723),  DqnV2_2i(234, 86),
	                 DqnV2_2i(211, 756), DqnV2_2i(752, 164), DqnV2_2i(516, 501), DqnV2_2i(64, 925),  DqnV2_2i(381, 581), DqnV2_2i(313, 650), DqnV2_2i(726, 599),
	                 DqnV2_2i(769, 638), DqnV2_2i(235, 590), DqnV2_2i(144, 162), DqnV2_2i(425, 757), DqnV2_2i(292, 692), DqnV2_2i(341, 219), DqnV2_2i(474, 282),
	                 DqnV2_2i(707, 241), DqnV2_2i(496, 348), DqnV2_2i(82, 712),  DqnV2_2i(31, 568),  DqnV2_2i(137, 411), DqnV2_2i(490, 753), DqnV2_2i(640, 343),
	                 DqnV2_2i(232, 208), DqnV2_2i(219, 553), DqnV2_2i(305, 600), DqnV2_2i(620, 18),  DqnV2_2i(133, 980), DqnV2_2i(725, 677), DqnV2_2i(204, 123),
	                 DqnV2_2i(289, 283), DqnV2_2i(345, 934), DqnV2_2i(503, 659), DqnV2_2i(724, 221), DqnV2_2i(600, 260), DqnV2_2i(177, 897), DqnV2_2i(442, 729),
	                 DqnV2_2i(420, 890), DqnV2_2i(620, 172), DqnV2_2i(363, 842), DqnV2_2i(165, 254), DqnV2_2i(258, 598), DqnV2_2i(685, 332), DqnV2_2i(781, 400),
	                 DqnV2_2i(63, 791),  DqnV2_2i(174, 447), DqnV2_2i(425, 788), DqnV2_2i(707, 406), DqnV2_2i(367, 204), DqnV2_2i(585, 410), DqnV2_2i(406, 550),
	                 DqnV2_2i(660, 601), DqnV2_2i(642, 685), DqnV2_2i(786, 129), DqnV2_2i(137, 144), DqnV2_2i(639, 420), DqnV2_2i(853, 289), DqnV2_2i(527, 185),
	                 DqnV2_2i(739, 440), DqnV2_2i(395, 42),  DqnV2_2i(390, 249), DqnV2_2i(721, 397), DqnV2_2i(64, 182),  DqnV2_2i(379, 540), DqnV2_2i(123, 328),
	                 DqnV2_2i(60, 904),  DqnV2_2i(762, 437), DqnV2_2i(24, 507),  DqnV2_2i(580, 330), DqnV2_2i(461, 220), DqnV2_2i(598, 99),  DqnV2_2i(632, 448),
	                 DqnV2_2i(389, 770), DqnV2_2i(279, 827), DqnV2_2i(586, 56),  DqnV2_2i(47, 643),  DqnV2_2i(173, 562), DqnV2_2i(111, 203), DqnV2_2i(84, 589),
	                 DqnV2_2i(476, 16),  DqnV2_2i(223, 91),  DqnV2_2i(772, 111), DqnV2_2i(818, 299), DqnV2_2i(139, 829), DqnV2_2i(455, 785), DqnV2_2i(627, 186),
	                 DqnV2_2i(338, 675), DqnV2_2i(800, 44),  DqnV2_2i(71, 865),  DqnV2_2i(44, 235),  DqnV2_2i(784, 299), DqnV2_2i(361, 910), DqnV2_2i(242, 261),
	                 DqnV2_2i(538, 687), DqnV2_2i(968, 179), DqnV2_2i(262, 504), DqnV2_2i(713, 645), DqnV2_2i(446, 107), DqnV2_2i(315, 723), DqnV2_2i(606, 41),
	                 DqnV2_2i(563, 605), DqnV2_2i(570, 249), DqnV2_2i(658, 265), DqnV2_2i(135, 356), DqnV2_2i(43, 301),  DqnV2_2i(140, 874), DqnV2_2i(584, 738),
	                 DqnV2_2i(780, 10),  DqnV2_2i(66, 505),  DqnV2_2i(204, 198), DqnV2_2i(318, 86),  DqnV2_2i(443, 718), DqnV2_2i(478, 430), DqnV2_2i(250, 85),
	                 DqnV2_2i(173, 168), DqnV2_2i(377, 210), DqnV2_2i(380, 887), DqnV2_2i(627, 560), DqnV2_2i(446, 98),  DqnV2_2i(374, 752), DqnV2_2i(682, 95),
	                 DqnV2_2i(864, 461), DqnV2_2i(468, 815), DqnV2_2i(653, 264), DqnV2_2i(552, 282), DqnV2_2i(376, 78),  DqnV2_2i(149, 449), DqnV2_2i(119, 748),
	                 DqnV2_2i(325, 919), DqnV2_2i(445, 543), DqnV2_2i(594, 686), DqnV2_2i(915, 72),  DqnV2_2i(87, 130),  DqnV2_2i(252, 356), DqnV2_2i(241, 494),
	                 DqnV2_2i(437, 886), DqnV2_2i(529, 344), DqnV2_2i(688, 564), DqnV2_2i(172, 293), DqnV2_2i(637, 659), DqnV2_2i(601, 417), DqnV2_2i(630, 371),
	                 DqnV2_2i(638, 228), DqnV2_2i(601, 291), DqnV2_2i(635, 295), DqnV2_2i(98, 289),  DqnV2_2i(513, 125), DqnV2_2i(715, 236), DqnV2_2i(441, 783),
	                 DqnV2_2i(506, 774), DqnV2_2i(305, 656), DqnV2_2i(56, 664),  DqnV2_2i(175, 645), DqnV2_2i(152, 320), DqnV2_2i(67, 818),  DqnV2_2i(224, 113),
	                 DqnV2_2i(15, 384),  DqnV2_2i(522, 358), DqnV2_2i(783, 165), DqnV2_2i(311, 910), DqnV2_2i(57, 472),  DqnV2_2i(154, 459), DqnV2_2i(49, 815),
	                 DqnV2_2i(577, 239), DqnV2_2i(745, 436), DqnV2_2i(715, 666), DqnV2_2i(804, 95),  DqnV2_2i(463, 473), DqnV2_2i(305, 95),  DqnV2_2i(262, 482),
	                 DqnV2_2i(438, 531), DqnV2_2i(329, 204), DqnV2_2i(242, 809), DqnV2_2i(737, 360), DqnV2_2i(321, 924), DqnV2_2i(623, 662), DqnV2_2i(608, 196),
	                 DqnV2_2i(337, 425), DqnV2_2i(543, 144), DqnV2_2i(217, 433), DqnV2_2i(561, 375), DqnV2_2i(80, 441),  DqnV2_2i(825, 202), DqnV2_2i(55, 879),
	                 DqnV2_2i(78, 310),  DqnV2_2i(255, 131), DqnV2_2i(207, 542), DqnV2_2i(243, 323), DqnV2_2i(759, 521), DqnV2_2i(536, 586), DqnV2_2i(133, 48),
	                 DqnV2_2i(926, 314), DqnV2_2i(845, 3),   DqnV2_2i(237, 472), DqnV2_2i(561, 106), DqnV2_2i(371, 455), DqnV2_2i(283, 786), DqnV2_2i(792, 102),
	                 DqnV2_2i(46, 583),  DqnV2_2i(589, 23),  DqnV2_2i(624, 601), DqnV2_2i(274, 744), DqnV2_2i(78, 681),  DqnV2_2i(559, 206), DqnV2_2i(914, 98),
	                 DqnV2_2i(273, 571), DqnV2_2i(105, 391), DqnV2_2i(592, 397), DqnV2_2i(295, 660), DqnV2_2i(519, 690), DqnV2_2i(430, 608), DqnV2_2i(399, 490),
	                 DqnV2_2i(197, 869), DqnV2_2i(537, 746), DqnV2_2i(84, 162),  DqnV2_2i(626, 313), DqnV2_2i(850, 472), DqnV2_2i(357, 207), DqnV2_2i(28, 14),
	                 DqnV2_2i(200, 285), DqnV2_2i(685, 459), DqnV2_2i(528, 741), DqnV2_2i(665, 594), DqnV2_2i(34, 317),  DqnV2_2i(786, 228), DqnV2_2i(182, 396),
	                 DqnV2_2i(111, 355), DqnV2_2i(540, 190), DqnV2_2i(648, 139), DqnV2_2i(3, 352),   DqnV2_2i(297, 394), DqnV2_2i(572, 785), DqnV2_2i(65, 751),
	                 DqnV2_2i(120, 738), DqnV2_2i(505, 432), DqnV2_2i(465, 801), DqnV2_2i(494, 274), DqnV2_2i(52, 293),  DqnV2_2i(832, 150), DqnV2_2i(241, 638),
	                 DqnV2_2i(579, 132), DqnV2_2i(692, 553), DqnV2_2i(343, 751), DqnV2_2i(802, 441), DqnV2_2i(218, 448), DqnV2_2i(623, 375), DqnV2_2i(225, 50),
	                 DqnV2_2i(601, 129), DqnV2_2i(678, 319), DqnV2_2i(165, 616), DqnV2_2i(529, 44),  DqnV2_2i(742, 255), DqnV2_2i(282, 395), DqnV2_2i(189, 129),
	                 DqnV2_2i(975, 204), DqnV2_2i(886, 254), DqnV2_2i(891, 82),  DqnV2_2i(723, 522), DqnV2_2i(390, 846), DqnV2_2i(382, 847), DqnV2_2i(833, 190),
	                 DqnV2_2i(317, 356), DqnV2_2i(259, 741), DqnV2_2i(95, 481),  DqnV2_2i(401, 324), DqnV2_2i(303, 557), DqnV2_2i(367, 854), DqnV2_2i(686, 464),
	                 DqnV2_2i(284, 485), DqnV2_2i(438, 555), DqnV2_2i(545, 268), DqnV2_2i(317, 489), DqnV2_2i(370, 760), DqnV2_2i(707, 446), DqnV2_2i(123, 451),
	                 DqnV2_2i(770, 365), DqnV2_2i(153, 932), DqnV2_2i(359, 21),  DqnV2_2i(489, 607), DqnV2_2i(103, 230), DqnV2_2i(697, 386), DqnV2_2i(129, 383),
	                 DqnV2_2i(360, 163), DqnV2_2i(876, 94),  DqnV2_2i(341, 649), DqnV2_2i(822, 459), DqnV2_2i(626, 587), DqnV2_2i(192, 971), DqnV2_2i(296, 23),
	                 DqnV2_2i(328, 587), DqnV2_2i(786, 466), DqnV2_2i(551, 526), DqnV2_2i(372, 57),  DqnV2_2i(273, 836), DqnV2_2i(466, 189), DqnV2_2i(891, 239),
	                 DqnV2_2i(56, 173),  DqnV2_2i(22, 942),  DqnV2_2i(760, 435), DqnV2_2i(746, 550), DqnV2_2i(839, 229), DqnV2_2i(521, 629), DqnV2_2i(639, 448),
	                 DqnV2_2i(255, 318), DqnV2_2i(261, 873), DqnV2_2i(333, 657), DqnV2_2i(151, 318), DqnV2_2i(927, 228), DqnV2_2i(517, 225), DqnV2_2i(521, 483),
	                 DqnV2_2i(616, 36),  DqnV2_2i(392, 561), DqnV2_2i(584, 710), DqnV2_2i(263, 70),  DqnV2_2i(555, 610), DqnV2_2i(844, 154), DqnV2_2i(253, 672),
	                 DqnV2_2i(7, 376),   DqnV2_2i(26, 320),  DqnV2_2i(339, 408), DqnV2_2i(573, 582), DqnV2_2i(126, 283), DqnV2_2i(323, 620), DqnV2_2i(495, 1),
	                 DqnV2_2i(682, 5),   DqnV2_2i(725, 455), DqnV2_2i(941, 275), DqnV2_2i(124, 782), DqnV2_2i(482, 535), DqnV2_2i(52, 505),  DqnV2_2i(235, 643),
	                 DqnV2_2i(519, 305), DqnV2_2i(718, 429), DqnV2_2i(237, 97),  DqnV2_2i(251, 689), DqnV2_2i(882, 16),  DqnV2_2i(34, 275),  DqnV2_2i(492, 587),
	                 DqnV2_2i(363, 889), DqnV2_2i(381, 59),  DqnV2_2i(531, 689), DqnV2_2i(937, 326), DqnV2_2i(398, 583), DqnV2_2i(756, 616), DqnV2_2i(25, 375),
	                 DqnV2_2i(585, 327), DqnV2_2i(605, 23),  DqnV2_2i(572, 189), DqnV2_2i(447, 599), DqnV2_2i(813, 78),  DqnV2_2i(409, 683), DqnV2_2i(695, 535),
	                 DqnV2_2i(24, 605),  DqnV2_2i(407, 318), DqnV2_2i(840, 150), DqnV2_2i(536, 826), DqnV2_2i(389, 498), DqnV2_2i(636, 273), DqnV2_2i(812, 558),
	                 DqnV2_2i(330, 429), DqnV2_2i(445, 228), DqnV2_2i(30, 242),  DqnV2_2i(239, 118), DqnV2_2i(897, 302), DqnV2_2i(22, 274),  DqnV2_2i(308, 229),
	                 DqnV2_2i(358, 263), DqnV2_2i(269, 619), DqnV2_2i(428, 43),  DqnV2_2i(218, 803), DqnV2_2i(341, 570), DqnV2_2i(406, 347), DqnV2_2i(942, 158),
	                 DqnV2_2i(158, 934), DqnV2_2i(66, 712),  DqnV2_2i(373, 826), DqnV2_2i(816, 293), DqnV2_2i(84, 545),  DqnV2_2i(107, 590), DqnV2_2i(129, 97),
	                 DqnV2_2i(59, 66),   DqnV2_2i(13, 785),  DqnV2_2i(125, 70),  DqnV2_2i(514, 704), DqnV2_2i(561, 176), DqnV2_2i(59, 616),  DqnV2_2i(565, 349),
	                 DqnV2_2i(341, 108), DqnV2_2i(576, 739), DqnV2_2i(105, 42),  DqnV2_2i(221, 712), DqnV2_2i(254, 850), DqnV2_2i(640, 527), DqnV2_2i(540, 24),
	                 DqnV2_2i(196, 407), DqnV2_2i(81, 193),  DqnV2_2i(647, 731), DqnV2_2i(74, 595),  DqnV2_2i(213, 901), DqnV2_2i(70, 600),  DqnV2_2i(11, 771),
	                 DqnV2_2i(683, 152), DqnV2_2i(333, 295), DqnV2_2i(514, 515), DqnV2_2i(583, 731), DqnV2_2i(81, 814),  DqnV2_2i(739, 275), DqnV2_2i(409, 324),
	                 DqnV2_2i(291, 787), DqnV2_2i(163, 534), DqnV2_2i(922, 238), DqnV2_2i(352, 152), DqnV2_2i(238, 131), DqnV2_2i(476, 592), DqnV2_2i(128, 328),
	                 DqnV2_2i(876, 263), DqnV2_2i(820, 234), DqnV2_2i(575, 655), DqnV2_2i(950, 145), DqnV2_2i(843, 275), DqnV2_2i(271, 611), DqnV2_2i(850, 423),
	                 DqnV2_2i(402, 596), DqnV2_2i(108, 690), DqnV2_2i(177, 288), DqnV2_2i(515, 827), DqnV2_2i(502, 402), DqnV2_2i(28, 526),  DqnV2_2i(15, 190),
	                 DqnV2_2i(62, 12),   DqnV2_2i(264, 229), DqnV2_2i(36, 437),  DqnV2_2i(634, 433), DqnV2_2i(485, 378), DqnV2_2i(154, 456), DqnV2_2i(515, 405),
	                 DqnV2_2i(272, 494), DqnV2_2i(869, 336), DqnV2_2i(240, 232), DqnV2_2i(903, 143), DqnV2_2i(892, 355), DqnV2_2i(375, 92),  DqnV2_2i(518, 161),
	                 DqnV2_2i(115, 137), DqnV2_2i(502, 289), DqnV2_2i(188, 22),  DqnV2_2i(289, 618), DqnV2_2i(264, 570), DqnV2_2i(482, 550), DqnV2_2i(90, 270),
	                 DqnV2_2i(509, 505), DqnV2_2i(388, 299), DqnV2_2i(559, 186), DqnV2_2i(76, 743),  DqnV2_2i(746, 515), DqnV2_2i(346, 36),  DqnV2_2i(309, 39),
	                 DqnV2_2i(184, 118), DqnV2_2i(883, 456), DqnV2_2i(82, 201),  DqnV2_2i(524, 322), DqnV2_2i(412, 309), DqnV2_2i(416, 383), DqnV2_2i(171, 479),
	                 DqnV2_2i(163, 413), DqnV2_2i(392, 317), DqnV2_2i(325, 910), DqnV2_2i(537, 579), DqnV2_2i(524, 418), DqnV2_2i(878, 461), DqnV2_2i(562, 132),
	                 DqnV2_2i(147, 649), DqnV2_2i(210, 279), DqnV2_2i(309, 563), DqnV2_2i(512, 368), DqnV2_2i(735, 258), DqnV2_2i(792, 54),  DqnV2_2i(639, 569),
	                 DqnV2_2i(152, 376), DqnV2_2i(333, 170), DqnV2_2i(873, 5),   DqnV2_2i(460, 449), DqnV2_2i(758, 531), DqnV2_2i(868, 346), DqnV2_2i(137, 238),
	                 DqnV2_2i(204, 66),  DqnV2_2i(93, 102),  DqnV2_2i(551, 720), DqnV2_2i(946, 219), DqnV2_2i(217, 601), DqnV2_2i(396, 249), DqnV2_2i(10, 732),
	                 DqnV2_2i(511, 723), DqnV2_2i(482, 632), DqnV2_2i(422, 397), DqnV2_2i(377, 474), DqnV2_2i(209, 644), DqnV2_2i(677, 43),  DqnV2_2i(706, 174),
	                 DqnV2_2i(849, 328), DqnV2_2i(322, 841), DqnV2_2i(62, 575),  DqnV2_2i(383, 666), DqnV2_2i(91, 582),  DqnV2_2i(260, 378), DqnV2_2i(267, 318),
	                 DqnV2_2i(247, 95),  DqnV2_2i(487, 870), DqnV2_2i(169, 811), DqnV2_2i(442, 671), DqnV2_2i(577, 89),  DqnV2_2i(255, 248), DqnV2_2i(207, 552),
	                 DqnV2_2i(221, 937), DqnV2_2i(844, 367), DqnV2_2i(783, 312), DqnV2_2i(378, 642), DqnV2_2i(520, 314), DqnV2_2i(29, 430),  DqnV2_2i(213, 403),
	                 DqnV2_2i(188, 9),   DqnV2_2i(635, 353), DqnV2_2i(759, 163), DqnV2_2i(221, 860), DqnV2_2i(399, 195), DqnV2_2i(279, 818), DqnV2_2i(154, 485),
	                 DqnV2_2i(762, 297), DqnV2_2i(349, 91),  DqnV2_2i(135, 745), DqnV2_2i(268, 791), DqnV2_2i(113, 490), DqnV2_2i(51, 54),   DqnV2_2i(285, 509),
	                 DqnV2_2i(618, 605), DqnV2_2i(341, 104), DqnV2_2i(422, 619), DqnV2_2i(327, 869), DqnV2_2i(554, 700), DqnV2_2i(28, 716),  DqnV2_2i(253, 511),
	                 DqnV2_2i(71, 791),  DqnV2_2i(497, 59),  DqnV2_2i(676, 218), DqnV2_2i(433, 661), DqnV2_2i(437, 207), DqnV2_2i(918, 351), DqnV2_2i(196, 842),
	                 DqnV2_2i(176, 901), DqnV2_2i(537, 63),  DqnV2_2i(274, 475), DqnV2_2i(207, 933), DqnV2_2i(399, 678), DqnV2_2i(520, 719), DqnV2_2i(45, 959),
	                 DqnV2_2i(661, 115), DqnV2_2i(275, 122), DqnV2_2i(505, 662), DqnV2_2i(21, 421),  DqnV2_2i(831, 458), DqnV2_2i(395, 422), DqnV2_2i(487, 197),
	                 DqnV2_2i(362, 412), DqnV2_2i(806, 354), DqnV2_2i(505, 35),  DqnV2_2i(525, 66),  DqnV2_2i(625, 344), DqnV2_2i(283, 685), DqnV2_2i(533, 608),
	                 DqnV2_2i(52, 562),  DqnV2_2i(216, 56),  DqnV2_2i(190, 107), DqnV2_2i(419, 266), DqnV2_2i(25, 325),  DqnV2_2i(425, 428), DqnV2_2i(438, 132),
	                 DqnV2_2i(659, 502), DqnV2_2i(89, 108),  DqnV2_2i(77, 536),  DqnV2_2i(743, 292), DqnV2_2i(12, 562),  DqnV2_2i(411, 186), DqnV2_2i(832, 305),
	                 DqnV2_2i(368, 424), DqnV2_2i(793, 602), DqnV2_2i(505, 634), DqnV2_2i(136, 154), DqnV2_2i(570, 122), DqnV2_2i(545, 748), DqnV2_2i(350, 237),
	                 DqnV2_2i(173, 235), DqnV2_2i(188, 659), DqnV2_2i(906, 407), DqnV2_2i(593, 511), DqnV2_2i(41, 959),  DqnV2_2i(647, 280), DqnV2_2i(433, 241),
	                 DqnV2_2i(302, 331), DqnV2_2i(137, 778), DqnV2_2i(698, 479), DqnV2_2i(559, 564), DqnV2_2i(149, 219), DqnV2_2i(592, 25),  DqnV2_2i(364, 42),
	                 DqnV2_2i(33, 781),  DqnV2_2i(731, 283), DqnV2_2i(484, 810), DqnV2_2i(608, 404), DqnV2_2i(8, 380),   DqnV2_2i(586, 660), DqnV2_2i(304, 313),
	                 DqnV2_2i(294, 790), DqnV2_2i(709, 501), DqnV2_2i(332, 380), DqnV2_2i(308, 269), DqnV2_2i(631, 329), DqnV2_2i(476, 654), DqnV2_2i(359, 755),
	                 DqnV2_2i(617, 295), DqnV2_2i(512, 793), DqnV2_2i(392, 858), DqnV2_2i(682, 317), DqnV2_2i(348, 747), DqnV2_2i(841, 56),  DqnV2_2i(610, 668),
	                 DqnV2_2i(647, 338), DqnV2_2i(394, 493), DqnV2_2i(439, 784), DqnV2_2i(569, 186), DqnV2_2i(418, 515), DqnV2_2i(347, 794), DqnV2_2i(384, 61),
	                 DqnV2_2i(746, 50),  DqnV2_2i(617, 573), DqnV2_2i(938, 162), DqnV2_2i(637, 42),  DqnV2_2i(140, 177), DqnV2_2i(729, 515), DqnV2_2i(646, 708),
	                 DqnV2_2i(790, 404), DqnV2_2i(64, 47),   DqnV2_2i(838, 292), DqnV2_2i(890, 68),  DqnV2_2i(508, 805), DqnV2_2i(78, 478),  DqnV2_2i(306, 180),
	                 DqnV2_2i(494, 13),  DqnV2_2i(223, 905), DqnV2_2i(285, 46),  DqnV2_2i(146, 145), DqnV2_2i(807, 52),  DqnV2_2i(557, 376), DqnV2_2i(680, 610),
	                 DqnV2_2i(960, 280), DqnV2_2i(919, 289), DqnV2_2i(840, 311), DqnV2_2i(947, 82),  DqnV2_2i(50, 554),  DqnV2_2i(772, 218), DqnV2_2i(339, 521),
	                 DqnV2_2i(392, 747), DqnV2_2i(377, 847), DqnV2_2i(651, 240), DqnV2_2i(27, 828),  DqnV2_2i(48, 749),  DqnV2_2i(579, 213), DqnV2_2i(131, 47),
	                 DqnV2_2i(277, 717), DqnV2_2i(176, 164), DqnV2_2i(216, 153), DqnV2_2i(225, 89),  DqnV2_2i(473, 379), DqnV2_2i(931, 131), DqnV2_2i(564, 611),
	                 DqnV2_2i(961, 220), DqnV2_2i(682, 659), DqnV2_2i(513, 34),  DqnV2_2i(619, 349), DqnV2_2i(706, 494), DqnV2_2i(699, 512), DqnV2_2i(258, 574),
	                 DqnV2_2i(30, 148),  DqnV2_2i(840, 52),  DqnV2_2i(267, 441), DqnV2_2i(48, 109),  DqnV2_2i(234, 145), DqnV2_2i(778, 623), DqnV2_2i(491, 10),
	                 DqnV2_2i(903, 52),  DqnV2_2i(132, 445), DqnV2_2i(543, 726), DqnV2_2i(859, 86),  DqnV2_2i(740, 455), DqnV2_2i(41, 84),   DqnV2_2i(282, 282),
	                 DqnV2_2i(381, 195), DqnV2_2i(551, 384), DqnV2_2i(47, 846),  DqnV2_2i(673, 65),  DqnV2_2i(192, 675), DqnV2_2i(149, 546), DqnV2_2i(66, 42),
	                 DqnV2_2i(9, 566),   DqnV2_2i(250, 320), DqnV2_2i(98, 614),  DqnV2_2i(657, 127), DqnV2_2i(395, 334), DqnV2_2i(353, 837), DqnV2_2i(707, 649),
	                 DqnV2_2i(181, 13),  DqnV2_2i(148, 408), DqnV2_2i(31, 333),  DqnV2_2i(154, 209), DqnV2_2i(760, 224), DqnV2_2i(311, 449), DqnV2_2i(663, 443),
	                 DqnV2_2i(604, 473), DqnV2_2i(128, 212), DqnV2_2i(529, 653), DqnV2_2i(78, 668),  DqnV2_2i(602, 25),  DqnV2_2i(403, 557), DqnV2_2i(632, 546),
	                 DqnV2_2i(139, 356), DqnV2_2i(154, 650), DqnV2_2i(479, 644), DqnV2_2i(412, 202), DqnV2_2i(602, 166), DqnV2_2i(327, 873), DqnV2_2i(413, 611),
	                 DqnV2_2i(175, 301), DqnV2_2i(616, 660), DqnV2_2i(658, 496), DqnV2_2i(131, 562), DqnV2_2i(302, 500), DqnV2_2i(618, 314), DqnV2_2i(487, 431),
	                 DqnV2_2i(163, 392), DqnV2_2i(102, 464), DqnV2_2i(372, 831), DqnV2_2i(801, 341), DqnV2_2i(720, 526), DqnV2_2i(911, 85),  DqnV2_2i(5, 820),
	                 DqnV2_2i(412, 270), DqnV2_2i(233, 175), DqnV2_2i(141, 275), DqnV2_2i(271, 151), DqnV2_2i(332, 263), DqnV2_2i(629, 753), DqnV2_2i(48, 496),
	                 DqnV2_2i(78, 108),  DqnV2_2i(779, 548), DqnV2_2i(263, 477), DqnV2_2i(771, 228), DqnV2_2i(190, 961), DqnV2_2i(449, 627), DqnV2_2i(328, 858),
	                 DqnV2_2i(17, 268),  DqnV2_2i(783, 107), DqnV2_2i(618, 148), DqnV2_2i(48, 894),  DqnV2_2i(424, 238), DqnV2_2i(942, 142), DqnV2_2i(691, 182),
	                 DqnV2_2i(359, 226), DqnV2_2i(500, 834), DqnV2_2i(404, 31),  DqnV2_2i(219, 330), DqnV2_2i(725, 526), DqnV2_2i(303, 623), DqnV2_2i(331, 643),
	                 DqnV2_2i(239, 909), DqnV2_2i(891, 342), DqnV2_2i(946, 273), DqnV2_2i(862, 423), DqnV2_2i(182, 198), DqnV2_2i(901, 426), DqnV2_2i(213, 477),
	                 DqnV2_2i(717, 146), DqnV2_2i(457, 743), DqnV2_2i(379, 367), DqnV2_2i(569, 88),  DqnV2_2i(320, 621), DqnV2_2i(209, 570), DqnV2_2i(501, 195),
	                 DqnV2_2i(250, 850), DqnV2_2i(113, 124), DqnV2_2i(182, 429), DqnV2_2i(656, 163), DqnV2_2i(470, 425), DqnV2_2i(250, 794), DqnV2_2i(508, 180),
	                 DqnV2_2i(858, 14),  DqnV2_2i(655, 737), DqnV2_2i(152, 763), DqnV2_2i(210, 213), DqnV2_2i(440, 603), DqnV2_2i(46, 403),  DqnV2_2i(330, 139),
	                 DqnV2_2i(556, 297), DqnV2_2i(467, 656), DqnV2_2i(422, 883), DqnV2_2i(247, 731), DqnV2_2i(133, 964), DqnV2_2i(175, 461), DqnV2_2i(185, 270),
	                 DqnV2_2i(381, 546), DqnV2_2i(409, 108), DqnV2_2i(102, 310), DqnV2_2i(404, 321), DqnV2_2i(249, 320), DqnV2_2i(600, 637), DqnV2_2i(417, 838),
	                 DqnV2_2i(309, 294), DqnV2_2i(374, 268), DqnV2_2i(915, 295), DqnV2_2i(467, 507), DqnV2_2i(644, 626), DqnV2_2i(326, 236), DqnV2_2i(381, 330),
	                 DqnV2_2i(77, 403),  DqnV2_2i(479, 786), DqnV2_2i(620, 2),   DqnV2_2i(503, 516), DqnV2_2i(162, 624), DqnV2_2i(261, 740), DqnV2_2i(340, 770),
	                 DqnV2_2i(545, 248), DqnV2_2i(81, 970),  DqnV2_2i(384, 89),  DqnV2_2i(691, 634), DqnV2_2i(325, 912), DqnV2_2i(312, 96),  DqnV2_2i(500, 601),
	                 DqnV2_2i(167, 363), DqnV2_2i(852, 220), DqnV2_2i(76, 940),  DqnV2_2i(539, 102), DqnV2_2i(156, 944), DqnV2_2i(372, 215), DqnV2_2i(453, 364),
	                 DqnV2_2i(446, 391), DqnV2_2i(145, 319), DqnV2_2i(543, 485), DqnV2_2i(14, 967),  DqnV2_2i(6, 281),   DqnV2_2i(749, 530), DqnV2_2i(124, 121),
	                 DqnV2_2i(211, 467), DqnV2_2i(175, 618), DqnV2_2i(121, 11),  DqnV2_2i(51, 981),  DqnV2_2i(584, 619), DqnV2_2i(484, 13),  DqnV2_2i(9, 962),
	                 DqnV2_2i(176, 499), DqnV2_2i(262, 715), DqnV2_2i(233, 180), DqnV2_2i(587, 351), DqnV2_2i(901, 126), DqnV2_2i(490, 432), DqnV2_2i(127, 541),
	                 DqnV2_2i(210, 492), DqnV2_2i(310, 96),  DqnV2_2i(221, 224), DqnV2_2i(12, 788),  DqnV2_2i(717, 447), DqnV2_2i(405, 225), DqnV2_2i(35, 692),
	                 DqnV2_2i(375, 253), DqnV2_2i(428, 116), DqnV2_2i(520, 739), DqnV2_2i(719, 461), DqnV2_2i(198, 308), DqnV2_2i(533, 234), DqnV2_2i(100, 547),
	                 DqnV2_2i(238, 831), DqnV2_2i(65, 659),  DqnV2_2i(22, 300),  DqnV2_2i(306, 817), DqnV2_2i(453, 360), DqnV2_2i(905, 157), DqnV2_2i(38, 993),
	                 DqnV2_2i(855, 416), DqnV2_2i(296, 642), DqnV2_2i(564, 576), DqnV2_2i(386, 263), DqnV2_2i(852, 368), DqnV2_2i(448, 377), DqnV2_2i(478, 474),
	                 DqnV2_2i(816, 535), DqnV2_2i(564, 447), DqnV2_2i(822, 420), DqnV2_2i(361, 461), DqnV2_2i(300, 729), DqnV2_2i(478, 816), DqnV2_2i(281, 201),
	                 DqnV2_2i(379, 448), DqnV2_2i(351, 619), DqnV2_2i(744, 617), DqnV2_2i(784, 444), DqnV2_2i(46, 28),   DqnV2_2i(90, 765),  DqnV2_2i(11, 248),
	                 DqnV2_2i(792, 422), DqnV2_2i(962, 171), DqnV2_2i(799, 204), DqnV2_2i(164, 878), DqnV2_2i(829, 58),  DqnV2_2i(231, 27),  DqnV2_2i(448, 650),
	                 DqnV2_2i(53, 712),  DqnV2_2i(279, 29),  DqnV2_2i(373, 1),   DqnV2_2i(284, 97),  DqnV2_2i(465, 239), DqnV2_2i(443, 641), DqnV2_2i(466, 661),
	                 DqnV2_2i(438, 582), DqnV2_2i(893, 199), DqnV2_2i(347, 856), DqnV2_2i(374, 926), DqnV2_2i(397, 295), DqnV2_2i(623, 589), DqnV2_2i(278, 577),
	                 DqnV2_2i(240, 948), DqnV2_2i(478, 286), DqnV2_2i(793, 18),  DqnV2_2i(282, 446), DqnV2_2i(555, 147), DqnV2_2i(142, 468), DqnV2_2i(948, 71),
	                 DqnV2_2i(532, 471), DqnV2_2i(792, 71),  DqnV2_2i(551, 506), DqnV2_2i(460, 513), DqnV2_2i(295, 203), DqnV2_2i(205, 556), DqnV2_2i(794, 587),
	                 DqnV2_2i(42, 701),  DqnV2_2i(738, 582), DqnV2_2i(691, 439), DqnV2_2i(823, 547), DqnV2_2i(86, 592),  DqnV2_2i(198, 615), DqnV2_2i(375, 287),
	                 DqnV2_2i(197, 205), DqnV2_2i(984, 153), DqnV2_2i(903, 322), DqnV2_2i(670, 236), DqnV2_2i(884, 205), DqnV2_2i(700, 5),   DqnV2_2i(596, 464),
	                 DqnV2_2i(167, 172), DqnV2_2i(436, 820), DqnV2_2i(380, 567), DqnV2_2i(63, 334),  DqnV2_2i(507, 250), DqnV2_2i(413, 901), DqnV2_2i(794, 143),
	                 DqnV2_2i(427, 703), DqnV2_2i(255, 14),  DqnV2_2i(125, 449), DqnV2_2i(284, 686), DqnV2_2i(695, 602), DqnV2_2i(748, 292), DqnV2_2i(630, 605),
	                 DqnV2_2i(109, 989), DqnV2_2i(322, 317), DqnV2_2i(392, 768), DqnV2_2i(420, 86),  DqnV2_2i(642, 420), DqnV2_2i(743, 198), DqnV2_2i(817, 527),
	                 DqnV2_2i(372, 33),  DqnV2_2i(71, 457),  DqnV2_2i(593, 736), DqnV2_2i(425, 536), DqnV2_2i(805, 534), DqnV2_2i(186, 660), DqnV2_2i(97, 436),
	                 DqnV2_2i(48, 367),  DqnV2_2i(371, 75),  DqnV2_2i(541, 388), DqnV2_2i(55, 798),  DqnV2_2i(402, 789), DqnV2_2i(228, 244), DqnV2_2i(387, 187),
	                 DqnV2_2i(736, 334), DqnV2_2i(578, 303), DqnV2_2i(257, 679), DqnV2_2i(426, 13),  DqnV2_2i(167, 226), DqnV2_2i(206, 176), DqnV2_2i(83, 178),
	                 DqnV2_2i(112, 661), DqnV2_2i(100, 67),  DqnV2_2i(216, 866), DqnV2_2i(178, 943), DqnV2_2i(594, 38),  DqnV2_2i(214, 524), DqnV2_2i(252, 78),
	                 DqnV2_2i(717, 168), DqnV2_2i(624, 110), DqnV2_2i(456, 185), DqnV2_2i(365, 900), DqnV2_2i(103, 526), DqnV2_2i(628, 239), DqnV2_2i(300, 786),
	                 DqnV2_2i(193, 225), DqnV2_2i(43, 977),  DqnV2_2i(687, 371), DqnV2_2i(235, 849), DqnV2_2i(41, 232),  DqnV2_2i(182, 187), DqnV2_2i(459, 420),
	                 DqnV2_2i(109, 820), DqnV2_2i(103, 93),  DqnV2_2i(240, 624), DqnV2_2i(150, 640), DqnV2_2i(28, 462),  DqnV2_2i(433, 862), DqnV2_2i(217, 714),
	                 DqnV2_2i(832, 363), DqnV2_2i(387, 672), DqnV2_2i(309, 356), DqnV2_2i(190, 214), DqnV2_2i(941, 168), DqnV2_2i(606, 61),  DqnV2_2i(9, 921),
	                 DqnV2_2i(404, 54),  DqnV2_2i(22, 620),  DqnV2_2i(292, 93),  DqnV2_2i(46, 354),  DqnV2_2i(213, 955), DqnV2_2i(63, 970),  DqnV2_2i(302, 794),
	                 DqnV2_2i(408, 741), DqnV2_2i(356, 662), DqnV2_2i(259, 513), DqnV2_2i(961, 6),   DqnV2_2i(132, 332), DqnV2_2i(138, 6),   DqnV2_2i(607, 13),
	                 DqnV2_2i(240, 526), DqnV2_2i(187, 558), DqnV2_2i(279, 295), DqnV2_2i(225, 611), DqnV2_2i(182, 620), DqnV2_2i(735, 504), DqnV2_2i(530, 666),
	                 DqnV2_2i(560, 812), DqnV2_2i(949, 11),  DqnV2_2i(822, 419), DqnV2_2i(424, 503), DqnV2_2i(98, 961),  DqnV2_2i(525, 714), DqnV2_2i(725, 444),
	                 DqnV2_2i(501, 604), DqnV2_2i(256, 269), DqnV2_2i(138, 883), DqnV2_2i(254, 671), DqnV2_2i(196, 365), DqnV2_2i(735, 595), DqnV2_2i(362, 525),
	                 DqnV2_2i(883, 128), DqnV2_2i(318, 869), DqnV2_2i(131, 80),  DqnV2_2i(731, 78),  DqnV2_2i(101, 931), DqnV2_2i(362, 780), DqnV2_2i(166, 258),
	                 DqnV2_2i(338, 709), DqnV2_2i(466, 202), DqnV2_2i(206, 541), DqnV2_2i(797, 590), DqnV2_2i(561, 645), DqnV2_2i(389, 215), DqnV2_2i(820, 286),
	                 DqnV2_2i(528, 3),   DqnV2_2i(308, 643), DqnV2_2i(616, 372), DqnV2_2i(674, 652), DqnV2_2i(342, 816), DqnV2_2i(92, 480),  DqnV2_2i(273, 97),
	                 DqnV2_2i(695, 199), DqnV2_2i(169, 712), DqnV2_2i(84, 911),  DqnV2_2i(769, 454), DqnV2_2i(474, 188), DqnV2_2i(901, 319), DqnV2_2i(344, 577),
	                 DqnV2_2i(320, 768), DqnV2_2i(712, 445), DqnV2_2i(574, 631), DqnV2_2i(243, 897), DqnV2_2i(275, 228), DqnV2_2i(222, 874), DqnV2_2i(48, 713),
	                 DqnV2_2i(436, 743), DqnV2_2i(150, 805), DqnV2_2i(875, 406), DqnV2_2i(396, 735), DqnV2_2i(331, 343), DqnV2_2i(418, 322), DqnV2_2i(279, 613),
	                 DqnV2_2i(189, 755), DqnV2_2i(773, 234), DqnV2_2i(750, 626), DqnV2_2i(562, 203), DqnV2_2i(146, 253), DqnV2_2i(758, 232), DqnV2_2i(934, 132),
	                 DqnV2_2i(589, 350), DqnV2_2i(757, 343), DqnV2_2i(594, 585), DqnV2_2i(580, 227), DqnV2_2i(542, 397), DqnV2_2i(225, 675), DqnV2_2i(300, 708),
	                 DqnV2_2i(387, 427), DqnV2_2i(489, 126), DqnV2_2i(53, 849),  DqnV2_2i(355, 355), DqnV2_2i(679, 551), DqnV2_2i(19, 416),  DqnV2_2i(937, 250),
	                 DqnV2_2i(619, 552), DqnV2_2i(124, 727), DqnV2_2i(210, 152), DqnV2_2i(85, 669),  DqnV2_2i(442, 394), DqnV2_2i(182, 933), DqnV2_2i(715, 614),
	                 DqnV2_2i(284, 516), DqnV2_2i(762, 624), DqnV2_2i(313, 883), DqnV2_2i(292, 121), DqnV2_2i(38, 242),  DqnV2_2i(56, 94),   DqnV2_2i(30, 415),
	                 DqnV2_2i(107, 34),  DqnV2_2i(542, 655), DqnV2_2i(389, 642), DqnV2_2i(265, 847), DqnV2_2i(89, 629),  DqnV2_2i(785, 418), DqnV2_2i(875, 75),
	                 DqnV2_2i(612, 780), DqnV2_2i(492, 679), DqnV2_2i(375, 464), DqnV2_2i(401, 189), DqnV2_2i(767, 555), DqnV2_2i(678, 638), DqnV2_2i(367, 39),
	                 DqnV2_2i(391, 303), DqnV2_2i(237, 796), DqnV2_2i(281, 709), DqnV2_2i(142, 354), DqnV2_2i(462, 167), DqnV2_2i(90, 973),  DqnV2_2i(232, 399),
	                 DqnV2_2i(362, 117), DqnV2_2i(997, 38),  DqnV2_2i(830, 528), DqnV2_2i(504, 363), DqnV2_2i(675, 342), DqnV2_2i(425, 768), DqnV2_2i(169, 770),
	                 DqnV2_2i(442, 842), DqnV2_2i(51, 304),  DqnV2_2i(56, 487),  DqnV2_2i(367, 869), DqnV2_2i(398, 149), DqnV2_2i(740, 508), DqnV2_2i(262, 793),
	                 DqnV2_2i(635, 591), DqnV2_2i(747, 568), DqnV2_2i(417, 68),  DqnV2_2i(376, 68),  DqnV2_2i(544, 817), DqnV2_2i(236, 111), DqnV2_2i(552, 213),
	                 DqnV2_2i(828, 518), DqnV2_2i(488, 1),   DqnV2_2i(596, 539), DqnV2_2i(8, 345),   DqnV2_2i(217, 751), DqnV2_2i(145, 713), DqnV2_2i(581, 806),
	                 DqnV2_2i(97, 810),  DqnV2_2i(16, 145),  DqnV2_2i(691, 13),  DqnV2_2i(257, 483), DqnV2_2i(228, 274), DqnV2_2i(912, 217), DqnV2_2i(539, 818),
	                 DqnV2_2i(195, 158), DqnV2_2i(84, 700),  DqnV2_2i(697, 241), DqnV2_2i(881, 138), DqnV2_2i(246, 848), DqnV2_2i(427, 586), DqnV2_2i(81, 703),
	                 DqnV2_2i(556, 547), DqnV2_2i(73, 582),  DqnV2_2i(571, 18),  DqnV2_2i(573, 284), DqnV2_2i(658, 445), DqnV2_2i(47, 98),   DqnV2_2i(51, 421),
	                 DqnV2_2i(675, 704), DqnV2_2i(132, 203), DqnV2_2i(530, 723), DqnV2_2i(755, 282), DqnV2_2i(114, 890), DqnV2_2i(335, 9),   DqnV2_2i(32, 470),
	                 DqnV2_2i(154, 843), DqnV2_2i(106, 927), DqnV2_2i(326, 118), DqnV2_2i(240, 586), DqnV2_2i(592, 456), DqnV2_2i(6, 68),    DqnV2_2i(190, 389),
	                 DqnV2_2i(445, 796), DqnV2_2i(178, 763), DqnV2_2i(868, 74),  DqnV2_2i(84, 836),  DqnV2_2i(56, 51),   DqnV2_2i(813, 34),  DqnV2_2i(328, 713),
	                 DqnV2_2i(628, 3),   DqnV2_2i(192, 192), DqnV2_2i(371, 869), DqnV2_2i(356, 899), DqnV2_2i(285, 55),  DqnV2_2i(991, 5),   DqnV2_2i(512, 679),
	                 DqnV2_2i(64, 482),  DqnV2_2i(896, 271), DqnV2_2i(325, 125), DqnV2_2i(113, 625), DqnV2_2i(78, 109),  DqnV2_2i(775, 501), DqnV2_2i(79, 260),
	                 DqnV2_2i(370, 392), DqnV2_2i(234, 778), DqnV2_2i(127, 940), DqnV2_2i(723, 527), DqnV2_2i(818, 498), DqnV2_2i(447, 170), DqnV2_2i(205, 583),
	                 DqnV2_2i(108, 719), DqnV2_2i(86, 897),  DqnV2_2i(163, 282), DqnV2_2i(377, 557), DqnV2_2i(281, 534), DqnV2_2i(734, 301), DqnV2_2i(774, 495),
	                 DqnV2_2i(559, 554), DqnV2_2i(107, 559), DqnV2_2i(503, 353), DqnV2_2i(151, 159), DqnV2_2i(70, 264),  DqnV2_2i(550, 742), DqnV2_2i(85, 158),
	                 DqnV2_2i(36, 76),   DqnV2_2i(128, 7),   DqnV2_2i(803, 520), DqnV2_2i(350, 100), DqnV2_2i(296, 721), DqnV2_2i(335, 455), DqnV2_2i(147, 696),
	                 DqnV2_2i(570, 542), DqnV2_2i(541, 59),  DqnV2_2i(464, 495), DqnV2_2i(743, 129), DqnV2_2i(234, 881), DqnV2_2i(718, 609), DqnV2_2i(511, 376),
	                 DqnV2_2i(143, 324), DqnV2_2i(986, 4),   DqnV2_2i(260, 111), DqnV2_2i(709, 657), DqnV2_2i(417, 264), DqnV2_2i(5, 111),   DqnV2_2i(951, 39),
	                 DqnV2_2i(66, 693),  DqnV2_2i(884, 112), DqnV2_2i(58, 289),  DqnV2_2i(256, 800), DqnV2_2i(449, 590), DqnV2_2i(386, 859), DqnV2_2i(537, 628),
	                 DqnV2_2i(626, 777), DqnV2_2i(221, 279), DqnV2_2i(346, 181), DqnV2_2i(265, 150), DqnV2_2i(518, 402), DqnV2_2i(813, 5),   DqnV2_2i(184, 468),
	                 DqnV2_2i(978, 193), DqnV2_2i(288, 126), DqnV2_2i(317, 871), DqnV2_2i(88, 456),  DqnV2_2i(240, 145), DqnV2_2i(173, 698), DqnV2_2i(368, 870),
	                 DqnV2_2i(33, 657),  DqnV2_2i(873, 469), DqnV2_2i(557, 630), DqnV2_2i(453, 237), DqnV2_2i(539, 45),  DqnV2_2i(482, 419), DqnV2_2i(85, 996),
	                 DqnV2_2i(145, 135), DqnV2_2i(458, 219), DqnV2_2i(47, 748),  DqnV2_2i(53, 928),  DqnV2_2i(704, 368), DqnV2_2i(205, 931), DqnV2_2i(46, 764),
	                 DqnV2_2i(112, 713), DqnV2_2i(224, 212), DqnV2_2i(206, 598), DqnV2_2i(439, 542), DqnV2_2i(432, 879), DqnV2_2i(257, 607), DqnV2_2i(208, 229),
	                 DqnV2_2i(178, 119), DqnV2_2i(452, 256), DqnV2_2i(215, 260), DqnV2_2i(501, 432), DqnV2_2i(14, 372),  DqnV2_2i(474, 375), DqnV2_2i(404, 61),
	                 DqnV2_2i(342, 169), DqnV2_2i(325, 889), DqnV2_2i(870, 204), DqnV2_2i(422, 835), DqnV2_2i(600, 581), DqnV2_2i(549, 245), DqnV2_2i(584, 767),
	                 DqnV2_2i(225, 650), DqnV2_2i(101, 852), DqnV2_2i(845, 226), DqnV2_2i(350, 764), DqnV2_2i(690, 665), DqnV2_2i(94, 443),  DqnV2_2i(334, 213),
	                 DqnV2_2i(185, 639), DqnV2_2i(525, 103), DqnV2_2i(26, 899),  DqnV2_2i(83, 43),   DqnV2_2i(75, 935),  DqnV2_2i(396, 34),  DqnV2_2i(450, 159),
	                 DqnV2_2i(636, 315), DqnV2_2i(657, 596), DqnV2_2i(414, 446), DqnV2_2i(348, 22),  DqnV2_2i(488, 861), DqnV2_2i(833, 431), DqnV2_2i(794, 129),
	                 DqnV2_2i(490, 709), DqnV2_2i(590, 157), DqnV2_2i(400, 44),  DqnV2_2i(89, 791),  DqnV2_2i(147, 539), DqnV2_2i(247, 748), DqnV2_2i(820, 140),
	                 DqnV2_2i(610, 425), DqnV2_2i(127, 519), DqnV2_2i(780, 519), DqnV2_2i(814, 190), DqnV2_2i(733, 169), DqnV2_2i(478, 567), DqnV2_2i(354, 770),
	                 DqnV2_2i(68, 348),  DqnV2_2i(244, 586), DqnV2_2i(228, 235), DqnV2_2i(9, 478),   DqnV2_2i(238, 188), DqnV2_2i(252, 555), DqnV2_2i(531, 603),
	                 DqnV2_2i(644, 716), DqnV2_2i(224, 470), DqnV2_2i(186, 32),  DqnV2_2i(816, 53),  DqnV2_2i(108, 836), DqnV2_2i(632, 713), DqnV2_2i(287, 782),
	                 DqnV2_2i(666, 377), DqnV2_2i(667, 65),  DqnV2_2i(81, 708),  DqnV2_2i(176, 103), DqnV2_2i(548, 320), DqnV2_2i(269, 959), DqnV2_2i(793, 190),
	                 DqnV2_2i(222, 608), DqnV2_2i(198, 671), DqnV2_2i(116, 821), DqnV2_2i(531, 267), DqnV2_2i(149, 951), DqnV2_2i(914, 216), DqnV2_2i(795, 382),
	                 DqnV2_2i(93, 532),  DqnV2_2i(921, 226), DqnV2_2i(171, 921), DqnV2_2i(156, 589), DqnV2_2i(247, 671), DqnV2_2i(231, 920), DqnV2_2i(332, 726),
	                 DqnV2_2i(672, 597), DqnV2_2i(147, 946), DqnV2_2i(130, 838), DqnV2_2i(240, 663), DqnV2_2i(643, 421), DqnV2_2i(239, 481), DqnV2_2i(182, 650),
	                 DqnV2_2i(281, 805), DqnV2_2i(458, 94),  DqnV2_2i(69, 877),  DqnV2_2i(874, 289), DqnV2_2i(385, 557), DqnV2_2i(187, 135), DqnV2_2i(298, 921),
	                 DqnV2_2i(645, 662), DqnV2_2i(85, 848),  DqnV2_2i(127, 917), DqnV2_2i(751, 186), DqnV2_2i(663, 312), DqnV2_2i(258, 252), DqnV2_2i(129, 49),
	                 DqnV2_2i(79, 943),  DqnV2_2i(8, 287),   DqnV2_2i(472, 211), DqnV2_2i(358, 8),   DqnV2_2i(400, 14),  DqnV2_2i(666, 152), DqnV2_2i(94, 190),
	                 DqnV2_2i(646, 205), DqnV2_2i(197, 937), DqnV2_2i(475, 369), DqnV2_2i(26, 209),  DqnV2_2i(381, 448), DqnV2_2i(333, 579), DqnV2_2i(66, 549),
	                 DqnV2_2i(494, 648), DqnV2_2i(738, 24),  DqnV2_2i(519, 287), DqnV2_2i(571, 335), DqnV2_2i(877, 252), DqnV2_2i(977, 46),  DqnV2_2i(452, 829),
	                 DqnV2_2i(99, 949),  DqnV2_2i(755, 455), DqnV2_2i(18, 871),  DqnV2_2i(207, 238), DqnV2_2i(860, 152), DqnV2_2i(17, 752),  DqnV2_2i(470, 517),
	                 DqnV2_2i(698, 6),   DqnV2_2i(645, 215), DqnV2_2i(301, 306), DqnV2_2i(210, 813), DqnV2_2i(551, 571), DqnV2_2i(416, 811), DqnV2_2i(292, 501),
	                 DqnV2_2i(296, 298), DqnV2_2i(811, 336), DqnV2_2i(787, 140), DqnV2_2i(711, 597), DqnV2_2i(491, 805), DqnV2_2i(696, 470), DqnV2_2i(505, 258),
	                 DqnV2_2i(24, 135),  DqnV2_2i(98, 484),  DqnV2_2i(541, 670), DqnV2_2i(607, 277), DqnV2_2i(67, 192),  DqnV2_2i(42, 472),  DqnV2_2i(166, 424),
	                 DqnV2_2i(792, 231), DqnV2_2i(842, 140), DqnV2_2i(409, 735), DqnV2_2i(828, 458), DqnV2_2i(666, 162), DqnV2_2i(621, 35),  DqnV2_2i(133, 806),
	                 DqnV2_2i(476, 559), DqnV2_2i(172, 908), DqnV2_2i(204, 181), DqnV2_2i(785, 151), DqnV2_2i(741, 301), DqnV2_2i(715, 485), DqnV2_2i(275, 297),
	                 DqnV2_2i(67, 938),  DqnV2_2i(196, 10),  DqnV2_2i(134, 651), DqnV2_2i(119, 560), DqnV2_2i(700, 265), DqnV2_2i(98, 375),  DqnV2_2i(35, 581),
	                 DqnV2_2i(461, 524), DqnV2_2i(659, 728), DqnV2_2i(318, 101), DqnV2_2i(228, 12),  DqnV2_2i(149, 679), DqnV2_2i(294, 64),  DqnV2_2i(74, 814),
	                 DqnV2_2i(156, 522), DqnV2_2i(757, 365), DqnV2_2i(438, 141), DqnV2_2i(695, 667), DqnV2_2i(64, 127),  DqnV2_2i(136, 310), DqnV2_2i(5, 188),
	                 DqnV2_2i(458, 814), DqnV2_2i(140, 176), DqnV2_2i(688, 713), DqnV2_2i(468, 539), DqnV2_2i(397, 671), DqnV2_2i(512, 657), DqnV2_2i(783, 62),
	                 DqnV2_2i(463, 38),  DqnV2_2i(612, 378), DqnV2_2i(335, 292), DqnV2_2i(193, 848), DqnV2_2i(107, 72),  DqnV2_2i(452, 385), DqnV2_2i(642, 531),
	                 DqnV2_2i(153, 195), DqnV2_2i(874, 4),   DqnV2_2i(748, 520), DqnV2_2i(56, 368),  DqnV2_2i(576, 99),  DqnV2_2i(526, 810), DqnV2_2i(800, 402),
	                 DqnV2_2i(514, 308), DqnV2_2i(593, 751), DqnV2_2i(695, 681), DqnV2_2i(303, 80),  DqnV2_2i(708, 57),  DqnV2_2i(7, 411),   DqnV2_2i(337, 665),
	                 DqnV2_2i(493, 698), DqnV2_2i(81, 654),  DqnV2_2i(62, 336),  DqnV2_2i(73, 965),  DqnV2_2i(403, 911), DqnV2_2i(366, 141), DqnV2_2i(358, 756),
	                 DqnV2_2i(154, 741), DqnV2_2i(533, 104), DqnV2_2i(98, 747),  DqnV2_2i(585, 441), DqnV2_2i(349, 850), DqnV2_2i(383, 519), DqnV2_2i(721, 158),
	                 DqnV2_2i(819, 560), DqnV2_2i(244, 779), DqnV2_2i(333, 636), DqnV2_2i(32, 548),  DqnV2_2i(98, 22),   DqnV2_2i(858, 455), DqnV2_2i(573, 423),
	                 DqnV2_2i(8, 994),   DqnV2_2i(397, 872), DqnV2_2i(868, 425), DqnV2_2i(526, 91),  DqnV2_2i(458, 579), DqnV2_2i(256, 849), DqnV2_2i(698, 394),
	                 DqnV2_2i(721, 588), DqnV2_2i(857, 451), DqnV2_2i(155, 224), DqnV2_2i(556, 785), DqnV2_2i(501, 515), DqnV2_2i(73, 454),  DqnV2_2i(101, 917),
	                 DqnV2_2i(596, 184), DqnV2_2i(220, 449), DqnV2_2i(137, 145), DqnV2_2i(260, 118), DqnV2_2i(523, 251), DqnV2_2i(856, 380), DqnV2_2i(884, 221),
	                 DqnV2_2i(414, 59),  DqnV2_2i(223, 615), DqnV2_2i(326, 496), DqnV2_2i(186, 225), DqnV2_2i(225, 864), DqnV2_2i(860, 185), DqnV2_2i(570, 684),
	                 DqnV2_2i(802, 223), DqnV2_2i(703, 543), DqnV2_2i(525, 107), DqnV2_2i(924, 224), DqnV2_2i(976, 202), DqnV2_2i(611, 229), DqnV2_2i(17, 937),
	                 DqnV2_2i(594, 597), DqnV2_2i(193, 172), DqnV2_2i(392, 582), DqnV2_2i(219, 938), DqnV2_2i(103, 236), DqnV2_2i(590, 229), DqnV2_2i(566, 813),
	                 DqnV2_2i(520, 57),  DqnV2_2i(181, 257), DqnV2_2i(33, 695),  DqnV2_2i(226, 699), DqnV2_2i(128, 730), DqnV2_2i(197, 526), DqnV2_2i(60, 244),
	                 DqnV2_2i(60, 439),  DqnV2_2i(235, 895), DqnV2_2i(630, 65),  DqnV2_2i(249, 812), DqnV2_2i(152, 843), DqnV2_2i(728, 611), DqnV2_2i(707, 279),
	                 DqnV2_2i(190, 277), DqnV2_2i(406, 591), DqnV2_2i(137, 221), DqnV2_2i(933, 121), DqnV2_2i(79, 531),  DqnV2_2i(235, 444), DqnV2_2i(524, 227),
	                 DqnV2_2i(836, 431), DqnV2_2i(95, 532),  DqnV2_2i(586, 132), DqnV2_2i(467, 68),  DqnV2_2i(139, 9),   DqnV2_2i(790, 16),  DqnV2_2i(288, 942),
	                 DqnV2_2i(407, 277), DqnV2_2i(650, 659), DqnV2_2i(429, 793), DqnV2_2i(140, 762), DqnV2_2i(507, 283), DqnV2_2i(469, 649), DqnV2_2i(270, 42),
	                 DqnV2_2i(489, 405), DqnV2_2i(766, 491), DqnV2_2i(725, 571), DqnV2_2i(97, 822),  DqnV2_2i(255, 506), DqnV2_2i(633, 454), DqnV2_2i(831, 496),
	                 DqnV2_2i(136, 602), DqnV2_2i(730, 23),  DqnV2_2i(347, 100), DqnV2_2i(637, 515), DqnV2_2i(782, 355), DqnV2_2i(301, 329), DqnV2_2i(228, 376),
	                 DqnV2_2i(74, 264),  DqnV2_2i(346, 344), DqnV2_2i(857, 241), DqnV2_2i(292, 262), DqnV2_2i(168, 570), DqnV2_2i(60, 556),  DqnV2_2i(833, 188),
	                 DqnV2_2i(878, 38),  DqnV2_2i(506, 55),  DqnV2_2i(328, 618), DqnV2_2i(578, 412), DqnV2_2i(780, 342), DqnV2_2i(282, 773), DqnV2_2i(674, 6),
	                 DqnV2_2i(810, 311), DqnV2_2i(395, 418), DqnV2_2i(96, 420),  DqnV2_2i(184, 194), DqnV2_2i(289, 48),  DqnV2_2i(512, 263), DqnV2_2i(414, 126),
	                 DqnV2_2i(100, 985), DqnV2_2i(519, 146), DqnV2_2i(925, 273), DqnV2_2i(62, 728),  DqnV2_2i(930, 225), DqnV2_2i(174, 575), DqnV2_2i(365, 821),
	                 DqnV2_2i(606, 68),  DqnV2_2i(336, 257), DqnV2_2i(594, 341), DqnV2_2i(834, 471), DqnV2_2i(335, 492), DqnV2_2i(740, 613), DqnV2_2i(489, 342),
	                 DqnV2_2i(269, 790), DqnV2_2i(61, 977),  DqnV2_2i(341, 328), DqnV2_2i(601, 415), DqnV2_2i(324, 689), DqnV2_2i(153, 820), DqnV2_2i(652, 80),
	                 DqnV2_2i(349, 622), DqnV2_2i(350, 664), DqnV2_2i(17, 711),  DqnV2_2i(631, 53),  DqnV2_2i(215, 681), DqnV2_2i(398, 235), DqnV2_2i(407, 866),
	                 DqnV2_2i(78, 362),  DqnV2_2i(470, 742), DqnV2_2i(594, 319), DqnV2_2i(102, 828), DqnV2_2i(137, 196), DqnV2_2i(503, 844), DqnV2_2i(146, 99),
	                 DqnV2_2i(654, 135), DqnV2_2i(15, 853),  DqnV2_2i(503, 25),  DqnV2_2i(892, 430), DqnV2_2i(36, 244),  DqnV2_2i(561, 809), DqnV2_2i(467, 19),
	                 DqnV2_2i(266, 520), DqnV2_2i(116, 134), DqnV2_2i(407, 722), DqnV2_2i(202, 23),  DqnV2_2i(409, 118), DqnV2_2i(823, 245), DqnV2_2i(458, 605),
	                 DqnV2_2i(2, 417),   DqnV2_2i(281, 820), DqnV2_2i(263, 279), DqnV2_2i(771, 603), DqnV2_2i(225, 574), DqnV2_2i(469, 113), DqnV2_2i(333, 193),
	                 DqnV2_2i(532, 489), DqnV2_2i(104, 421), DqnV2_2i(400, 253), DqnV2_2i(722, 596), DqnV2_2i(16, 95),   DqnV2_2i(336, 631), DqnV2_2i(266, 28),
	                 DqnV2_2i(725, 155), DqnV2_2i(32, 7),    DqnV2_2i(333, 435), DqnV2_2i(184, 404), DqnV2_2i(75, 524),  DqnV2_2i(549, 512), DqnV2_2i(488, 749),
	                 DqnV2_2i(843, 536), DqnV2_2i(882, 279), DqnV2_2i(547, 584), DqnV2_2i(227, 285), DqnV2_2i(190, 351), DqnV2_2i(71, 234),  DqnV2_2i(531, 622),
	                 DqnV2_2i(556, 500), DqnV2_2i(369, 118), DqnV2_2i(648, 582), DqnV2_2i(259, 616), DqnV2_2i(672, 616), DqnV2_2i(107, 656), DqnV2_2i(258, 263),
	                 DqnV2_2i(198, 137), DqnV2_2i(769, 423), DqnV2_2i(806, 1),   DqnV2_2i(15, 358),  DqnV2_2i(982, 148), DqnV2_2i(316, 386), DqnV2_2i(972, 119),
	                 DqnV2_2i(471, 116), DqnV2_2i(849, 163), DqnV2_2i(299, 79),  DqnV2_2i(562, 148), DqnV2_2i(71, 895),  DqnV2_2i(17, 661),  DqnV2_2i(395, 849),
	                 DqnV2_2i(561, 126), DqnV2_2i(829, 233), DqnV2_2i(391, 119), DqnV2_2i(983, 50),  DqnV2_2i(745, 485), DqnV2_2i(360, 432), DqnV2_2i(407, 647),
	                 DqnV2_2i(19, 828),  DqnV2_2i(157, 876), DqnV2_2i(506, 163), DqnV2_2i(194, 851), DqnV2_2i(477, 500), DqnV2_2i(63, 945),  DqnV2_2i(452, 571),
	                 DqnV2_2i(293, 368), DqnV2_2i(281, 672), DqnV2_2i(600, 258), DqnV2_2i(792, 193), DqnV2_2i(111, 868), DqnV2_2i(93, 928),  DqnV2_2i(936, 346),
	                 DqnV2_2i(625, 175), DqnV2_2i(324, 855), DqnV2_2i(902, 35),  DqnV2_2i(53, 908),  DqnV2_2i(519, 257), DqnV2_2i(900, 149), DqnV2_2i(664, 146),
	                 DqnV2_2i(736, 12),  DqnV2_2i(47, 94),   DqnV2_2i(214, 56),  DqnV2_2i(494, 705), DqnV2_2i(463, 400), DqnV2_2i(302, 666), DqnV2_2i(157, 950),
	                 DqnV2_2i(112, 878), DqnV2_2i(333, 430), DqnV2_2i(594, 590), DqnV2_2i(295, 867), DqnV2_2i(207, 681), DqnV2_2i(421, 377), DqnV2_2i(214, 551),
	                 DqnV2_2i(536, 61),  DqnV2_2i(704, 586), DqnV2_2i(362, 641), DqnV2_2i(677, 556), DqnV2_2i(639, 433), DqnV2_2i(453, 619), DqnV2_2i(452, 670),
	                 DqnV2_2i(177, 893), DqnV2_2i(259, 321), DqnV2_2i(606, 326), DqnV2_2i(242, 238), DqnV2_2i(594, 256), DqnV2_2i(636, 72),  DqnV2_2i(158, 377),
	                 DqnV2_2i(598, 77),  DqnV2_2i(178, 177), DqnV2_2i(783, 604), DqnV2_2i(193, 785), DqnV2_2i(204, 553), DqnV2_2i(295, 443), DqnV2_2i(208, 92),
	                 DqnV2_2i(38, 308),  DqnV2_2i(631, 108), DqnV2_2i(223, 840), DqnV2_2i(742, 68),  DqnV2_2i(296, 247), DqnV2_2i(749, 80),  DqnV2_2i(184, 327),
	                 DqnV2_2i(680, 590), DqnV2_2i(886, 390), DqnV2_2i(712, 645), DqnV2_2i(207, 212), DqnV2_2i(465, 459), DqnV2_2i(518, 846), DqnV2_2i(223, 496),
	                 DqnV2_2i(4, 474),   DqnV2_2i(1, 892),   DqnV2_2i(274, 314), DqnV2_2i(895, 59),  DqnV2_2i(86, 266),  DqnV2_2i(243, 185), DqnV2_2i(356, 466),
	                 DqnV2_2i(699, 115), DqnV2_2i(517, 119), DqnV2_2i(602, 473), DqnV2_2i(232, 954), DqnV2_2i(420, 699), DqnV2_2i(784, 498), DqnV2_2i(57, 365),
	                 DqnV2_2i(817, 165), DqnV2_2i(219, 906), DqnV2_2i(91, 47),   DqnV2_2i(249, 236), DqnV2_2i(640, 558), DqnV2_2i(888, 169), DqnV2_2i(818, 6),
	                 DqnV2_2i(896, 355), DqnV2_2i(219, 160), DqnV2_2i(662, 735), DqnV2_2i(19, 695),  DqnV2_2i(879, 330), DqnV2_2i(666, 218), DqnV2_2i(885, 406),
	                 DqnV2_2i(916, 137), DqnV2_2i(380, 732), DqnV2_2i(133, 159), DqnV2_2i(485, 391), DqnV2_2i(720, 665), DqnV2_2i(813, 405), DqnV2_2i(183, 178),
	                 DqnV2_2i(577, 587), DqnV2_2i(357, 896), DqnV2_2i(320, 282), DqnV2_2i(153, 674), DqnV2_2i(670, 298), DqnV2_2i(478, 618), DqnV2_2i(69, 522),
	                 DqnV2_2i(367, 899), DqnV2_2i(366, 892), DqnV2_2i(73, 60),   DqnV2_2i(641, 216), DqnV2_2i(555, 259), DqnV2_2i(15, 839),  DqnV2_2i(318, 239),
	                 DqnV2_2i(634, 328), DqnV2_2i(675, 174), DqnV2_2i(257, 706), DqnV2_2i(225, 421), DqnV2_2i(552, 170), DqnV2_2i(695, 550), DqnV2_2i(122, 623),
	                 DqnV2_2i(920, 283), DqnV2_2i(241, 755), DqnV2_2i(533, 651), DqnV2_2i(426, 666), DqnV2_2i(650, 616), DqnV2_2i(980, 94),  DqnV2_2i(153, 509),
	                 DqnV2_2i(50, 604),  DqnV2_2i(326, 537), DqnV2_2i(608, 139), DqnV2_2i(982, 37),  DqnV2_2i(338, 396), DqnV2_2i(732, 573), DqnV2_2i(595, 748),
	                 DqnV2_2i(594, 742), DqnV2_2i(607, 420), DqnV2_2i(19, 359),  DqnV2_2i(808, 231), DqnV2_2i(702, 680), DqnV2_2i(453, 169), DqnV2_2i(60, 311),
	                 DqnV2_2i(254, 690), DqnV2_2i(307, 545), DqnV2_2i(950, 239), DqnV2_2i(689, 102), DqnV2_2i(609, 361), DqnV2_2i(724, 688), DqnV2_2i(16, 579),
	                 DqnV2_2i(514, 812), DqnV2_2i(469, 557), DqnV2_2i(171, 738), DqnV2_2i(750, 460), DqnV2_2i(10, 163),  DqnV2_2i(705, 607), DqnV2_2i(475, 384),
	                 DqnV2_2i(430, 539), DqnV2_2i(621, 267), DqnV2_2i(953, 260), DqnV2_2i(377, 248), DqnV2_2i(495, 672), DqnV2_2i(626, 716), DqnV2_2i(224, 648),
	                 DqnV2_2i(267, 756), DqnV2_2i(379, 709), DqnV2_2i(119, 512), DqnV2_2i(130, 895), DqnV2_2i(469, 558), DqnV2_2i(141, 673), DqnV2_2i(298, 529),
	                 DqnV2_2i(305, 491), DqnV2_2i(674, 392), DqnV2_2i(291, 401), DqnV2_2i(724, 584), DqnV2_2i(78, 29),   DqnV2_2i(428, 287), DqnV2_2i(803, 232),
	                 DqnV2_2i(342, 122), DqnV2_2i(474, 661), DqnV2_2i(238, 701), DqnV2_2i(484, 107), DqnV2_2i(391, 893), DqnV2_2i(105, 470), DqnV2_2i(513, 433),
	                 DqnV2_2i(268, 250), DqnV2_2i(488, 482), DqnV2_2i(332, 766), DqnV2_2i(584, 660), DqnV2_2i(280, 417), DqnV2_2i(178, 576), DqnV2_2i(469, 175),
	                 DqnV2_2i(383, 334), DqnV2_2i(218, 390), DqnV2_2i(671, 739), DqnV2_2i(583, 650), DqnV2_2i(20, 44),   DqnV2_2i(532, 261), DqnV2_2i(924, 256),
	                 DqnV2_2i(650, 755), DqnV2_2i(4, 235),   DqnV2_2i(342, 885), DqnV2_2i(51, 917),  DqnV2_2i(501, 18),  DqnV2_2i(507, 729), DqnV2_2i(454, 436),
	                 DqnV2_2i(905, 169), DqnV2_2i(41, 275),  DqnV2_2i(132, 384), DqnV2_2i(11, 917),  DqnV2_2i(365, 389), DqnV2_2i(454, 195), DqnV2_2i(696, 144),
	                 DqnV2_2i(427, 419), DqnV2_2i(704, 224), DqnV2_2i(515, 53),  DqnV2_2i(597, 73),  DqnV2_2i(522, 659), DqnV2_2i(495, 846), DqnV2_2i(107, 816),
	                 DqnV2_2i(110, 286), DqnV2_2i(747, 46),  DqnV2_2i(151, 331), DqnV2_2i(387, 46),  DqnV2_2i(406, 708), DqnV2_2i(336, 355), DqnV2_2i(227, 213),
	                 DqnV2_2i(391, 262), DqnV2_2i(467, 780), DqnV2_2i(53, 964),  DqnV2_2i(671, 20),  DqnV2_2i(94, 278),  DqnV2_2i(264, 767), DqnV2_2i(34, 267),
	                 DqnV2_2i(784, 535), DqnV2_2i(857, 347), DqnV2_2i(56, 13),   DqnV2_2i(392, 57),  DqnV2_2i(651, 749), DqnV2_2i(815, 337), DqnV2_2i(365, 561),
	                 DqnV2_2i(369, 835), DqnV2_2i(752, 157), DqnV2_2i(517, 774), DqnV2_2i(279, 494), DqnV2_2i(366, 922), DqnV2_2i(414, 747), DqnV2_2i(691, 695),
	                 DqnV2_2i(837, 498), DqnV2_2i(732, 298), DqnV2_2i(68, 96),   DqnV2_2i(96, 891),  DqnV2_2i(74, 888),  DqnV2_2i(574, 379), DqnV2_2i(2, 878),
	                 DqnV2_2i(883, 323), DqnV2_2i(384, 84),  DqnV2_2i(594, 248), DqnV2_2i(305, 236), DqnV2_2i(433, 630), DqnV2_2i(286, 192), DqnV2_2i(868, 243),
	                 DqnV2_2i(398, 327), DqnV2_2i(241, 835), DqnV2_2i(86, 713),  DqnV2_2i(108, 675), DqnV2_2i(384, 214), DqnV2_2i(556, 565), DqnV2_2i(747, 453),
	                 DqnV2_2i(626, 364), DqnV2_2i(97, 266),  DqnV2_2i(783, 228), DqnV2_2i(612, 406), DqnV2_2i(847, 135), DqnV2_2i(671, 656), DqnV2_2i(901, 56),
	                 DqnV2_2i(545, 688), DqnV2_2i(560, 411), DqnV2_2i(356, 93),  DqnV2_2i(32, 9),    DqnV2_2i(265, 280), DqnV2_2i(410, 899), DqnV2_2i(155, 449),
	                 DqnV2_2i(395, 553), DqnV2_2i(640, 452), DqnV2_2i(717, 668), DqnV2_2i(514, 35),  DqnV2_2i(680, 551), DqnV2_2i(700, 326), DqnV2_2i(417, 511),
	                 DqnV2_2i(584, 258), DqnV2_2i(762, 137), DqnV2_2i(782, 412), DqnV2_2i(631, 725), DqnV2_2i(399, 505), DqnV2_2i(835, 449), DqnV2_2i(752, 133),
	                 DqnV2_2i(61, 442),  DqnV2_2i(717, 159), DqnV2_2i(736, 309), DqnV2_2i(458, 369), DqnV2_2i(591, 493), DqnV2_2i(351, 788), DqnV2_2i(179, 795),
	                 DqnV2_2i(932, 43),  DqnV2_2i(513, 337), DqnV2_2i(448, 554), DqnV2_2i(374, 386), DqnV2_2i(831, 488), DqnV2_2i(761, 182), DqnV2_2i(862, 504),
	                 DqnV2_2i(12, 599),  DqnV2_2i(374, 917), DqnV2_2i(25, 798),  DqnV2_2i(813, 87),  DqnV2_2i(429, 734), DqnV2_2i(190, 323), DqnV2_2i(460, 168),
	                 DqnV2_2i(757, 103), DqnV2_2i(725, 218), DqnV2_2i(313, 37),  DqnV2_2i(272, 706), DqnV2_2i(54, 266),  DqnV2_2i(123, 48),  DqnV2_2i(604, 761),
	                 DqnV2_2i(189, 576), DqnV2_2i(165, 198), DqnV2_2i(680, 706), DqnV2_2i(798, 68),  DqnV2_2i(664, 104), DqnV2_2i(363, 450), DqnV2_2i(391, 527),
	                 DqnV2_2i(132, 907), DqnV2_2i(450, 145), DqnV2_2i(924, 217), DqnV2_2i(888, 73),  DqnV2_2i(755, 189), DqnV2_2i(253, 966), DqnV2_2i(416, 637),
	                 DqnV2_2i(743, 445), DqnV2_2i(271, 244), DqnV2_2i(171, 887), DqnV2_2i(850, 469), DqnV2_2i(363, 194), DqnV2_2i(431, 621), DqnV2_2i(350, 298),
	                 DqnV2_2i(413, 58),  DqnV2_2i(236, 778), DqnV2_2i(798, 511), DqnV2_2i(131, 970), DqnV2_2i(13, 652),  DqnV2_2i(360, 745), DqnV2_2i(683, 594),
	                 DqnV2_2i(171, 764), DqnV2_2i(765, 218), DqnV2_2i(567, 575), DqnV2_2i(530, 332), DqnV2_2i(265, 393), DqnV2_2i(543, 504), DqnV2_2i(715, 15),
	                 DqnV2_2i(269, 858), DqnV2_2i(675, 554), DqnV2_2i(714, 586), DqnV2_2i(513, 295), DqnV2_2i(634, 157), DqnV2_2i(331, 941), DqnV2_2i(426, 16),
	                 DqnV2_2i(311, 336), DqnV2_2i(700, 531), DqnV2_2i(107, 676), DqnV2_2i(297, 329), DqnV2_2i(358, 15),  DqnV2_2i(635, 437), DqnV2_2i(427, 303),
	                 DqnV2_2i(754, 115), DqnV2_2i(538, 836), DqnV2_2i(510, 131), DqnV2_2i(539, 544), DqnV2_2i(457, 771), DqnV2_2i(329, 401), DqnV2_2i(859, 369),
	                 DqnV2_2i(569, 39),  DqnV2_2i(35, 770),  DqnV2_2i(737, 528), DqnV2_2i(207, 304), DqnV2_2i(34, 720),  DqnV2_2i(441, 855), DqnV2_2i(784, 381),
	                 DqnV2_2i(268, 257), DqnV2_2i(987, 39),  DqnV2_2i(98, 728),  DqnV2_2i(361, 210), DqnV2_2i(426, 549), DqnV2_2i(226, 872), DqnV2_2i(15, 978),
	                 DqnV2_2i(941, 72),  DqnV2_2i(948, 178), DqnV2_2i(567, 631), DqnV2_2i(924, 171), DqnV2_2i(427, 665), DqnV2_2i(237, 882), DqnV2_2i(265, 626),
	                 DqnV2_2i(473, 385), DqnV2_2i(233, 41),  DqnV2_2i(555, 376), DqnV2_2i(98, 583),  DqnV2_2i(66, 165),  DqnV2_2i(369, 60),  DqnV2_2i(85, 559),
	                 DqnV2_2i(592, 633), DqnV2_2i(722, 474), DqnV2_2i(194, 264), DqnV2_2i(727, 236), DqnV2_2i(464, 419), DqnV2_2i(830, 102), DqnV2_2i(654, 613),
	                 DqnV2_2i(304, 743), DqnV2_2i(718, 195), DqnV2_2i(740, 251), DqnV2_2i(242, 827), DqnV2_2i(79, 597),  DqnV2_2i(862, 239), DqnV2_2i(237, 134),
	                 DqnV2_2i(245, 599), DqnV2_2i(338, 149), DqnV2_2i(291, 376), DqnV2_2i(498, 625), DqnV2_2i(14, 677),  DqnV2_2i(695, 331), DqnV2_2i(435, 406),
	                 DqnV2_2i(946, 74),  DqnV2_2i(285, 484), DqnV2_2i(105, 280), DqnV2_2i(380, 118), DqnV2_2i(506, 663), DqnV2_2i(564, 613), DqnV2_2i(429, 200),
	                 DqnV2_2i(166, 286), DqnV2_2i(323, 200), DqnV2_2i(821, 121), DqnV2_2i(696, 683), DqnV2_2i(764, 190), DqnV2_2i(298, 603), DqnV2_2i(60, 927),
	                 DqnV2_2i(105, 168), DqnV2_2i(341, 29),  DqnV2_2i(981, 176), DqnV2_2i(115, 356), DqnV2_2i(252, 617), DqnV2_2i(925, 152), DqnV2_2i(833, 485),
	                 DqnV2_2i(470, 18),  DqnV2_2i(205, 494), DqnV2_2i(512, 595), DqnV2_2i(394, 384), DqnV2_2i(515, 400), DqnV2_2i(854, 338), DqnV2_2i(79, 74),
	                 DqnV2_2i(966, 166), DqnV2_2i(349, 920), DqnV2_2i(685, 20),  DqnV2_2i(407, 359), DqnV2_2i(358, 467), DqnV2_2i(258, 265), DqnV2_2i(293, 9),
	                 DqnV2_2i(567, 20),  DqnV2_2i(139, 860), DqnV2_2i(44, 516),  DqnV2_2i(246, 679), DqnV2_2i(929, 21),  DqnV2_2i(5, 152),   DqnV2_2i(318, 57),
	                 DqnV2_2i(70, 397),  DqnV2_2i(298, 533), DqnV2_2i(41, 155),  DqnV2_2i(419, 563), DqnV2_2i(168, 360), DqnV2_2i(598, 564), DqnV2_2i(21, 708),
	                 DqnV2_2i(215, 179), DqnV2_2i(430, 242), DqnV2_2i(7, 114),   DqnV2_2i(625, 572), DqnV2_2i(602, 147), DqnV2_2i(311, 80),  DqnV2_2i(791, 422),
	                 DqnV2_2i(971, 134), DqnV2_2i(400, 678), DqnV2_2i(439, 385), DqnV2_2i(565, 742), DqnV2_2i(98, 433),  DqnV2_2i(249, 891), DqnV2_2i(756, 382),
	                 DqnV2_2i(445, 34),  DqnV2_2i(757, 637), DqnV2_2i(634, 646), DqnV2_2i(202, 885), DqnV2_2i(421, 864), DqnV2_2i(200, 664), DqnV2_2i(717, 409),
	                 DqnV2_2i(413, 807), DqnV2_2i(658, 16),  DqnV2_2i(102, 905), DqnV2_2i(278, 337), DqnV2_2i(256, 664), DqnV2_2i(210, 658), DqnV2_2i(153, 942),
	                 DqnV2_2i(780, 168), DqnV2_2i(443, 526), DqnV2_2i(899, 408), DqnV2_2i(148, 955), DqnV2_2i(219, 723), DqnV2_2i(322, 835), DqnV2_2i(104, 448),
	                 DqnV2_2i(80, 809),  DqnV2_2i(608, 578), DqnV2_2i(125, 846), DqnV2_2i(62, 309),  DqnV2_2i(536, 328), DqnV2_2i(588, 83),  DqnV2_2i(502, 54),
	                 DqnV2_2i(583, 389), DqnV2_2i(265, 684), DqnV2_2i(594, 304), DqnV2_2i(810, 107), DqnV2_2i(684, 410), DqnV2_2i(31, 34),   DqnV2_2i(872, 247),
	                 DqnV2_2i(407, 116), DqnV2_2i(461, 855), DqnV2_2i(569, 680), DqnV2_2i(501, 552), DqnV2_2i(378, 160), DqnV2_2i(208, 140), DqnV2_2i(506, 191),
	                 DqnV2_2i(238, 434), DqnV2_2i(52, 171),  DqnV2_2i(181, 17),  DqnV2_2i(809, 541), DqnV2_2i(737, 127), DqnV2_2i(107, 895), DqnV2_2i(511, 347),
	                 DqnV2_2i(678, 500), DqnV2_2i(6, 951),   DqnV2_2i(651, 555), DqnV2_2i(767, 413), DqnV2_2i(869, 29),  DqnV2_2i(474, 572), DqnV2_2i(71, 608),
	                 DqnV2_2i(620, 585), DqnV2_2i(11, 987),  DqnV2_2i(958, 261), DqnV2_2i(807, 36),  DqnV2_2i(617, 618), DqnV2_2i(412, 788), DqnV2_2i(352, 769),
	                 DqnV2_2i(241, 944), DqnV2_2i(382, 495), DqnV2_2i(846, 363), DqnV2_2i(316, 551), DqnV2_2i(870, 324), DqnV2_2i(240, 361), DqnV2_2i(575, 780),
	                 DqnV2_2i(80, 320),  DqnV2_2i(614, 502), DqnV2_2i(372, 813), DqnV2_2i(835, 4),   DqnV2_2i(739, 136), DqnV2_2i(70, 578),  DqnV2_2i(891, 437),
	                 DqnV2_2i(578, 416), DqnV2_2i(635, 731), DqnV2_2i(645, 209), DqnV2_2i(354, 599), DqnV2_2i(746, 595), DqnV2_2i(215, 640), DqnV2_2i(189, 355),
	                 DqnV2_2i(185, 591), DqnV2_2i(814, 214), DqnV2_2i(474, 152), DqnV2_2i(588, 718), DqnV2_2i(594, 22),  DqnV2_2i(876, 58),  DqnV2_2i(239, 163),
	                 DqnV2_2i(463, 250), DqnV2_2i(297, 797), DqnV2_2i(364, 684), DqnV2_2i(348, 81),  DqnV2_2i(183, 307), DqnV2_2i(161, 413), DqnV2_2i(222, 320),
	                 DqnV2_2i(665, 60),  DqnV2_2i(621, 778), DqnV2_2i(144, 81),  DqnV2_2i(306, 178), DqnV2_2i(201, 536), DqnV2_2i(284, 319), DqnV2_2i(69, 888),
	                 DqnV2_2i(331, 363), DqnV2_2i(154, 367), DqnV2_2i(372, 868), DqnV2_2i(19, 823),  DqnV2_2i(977, 167), DqnV2_2i(360, 341), DqnV2_2i(599, 449),
	                 DqnV2_2i(97, 73),   DqnV2_2i(776, 76),  DqnV2_2i(110, 430), DqnV2_2i(220, 943), DqnV2_2i(765, 146), DqnV2_2i(85, 464),  DqnV2_2i(928, 202),
	                 DqnV2_2i(650, 130), DqnV2_2i(591, 311), DqnV2_2i(523, 733), DqnV2_2i(236, 519), DqnV2_2i(12, 283),  DqnV2_2i(125, 254), DqnV2_2i(116, 432),
	                 DqnV2_2i(74, 769),  DqnV2_2i(333, 788), DqnV2_2i(49, 60),   DqnV2_2i(586, 505), DqnV2_2i(813, 355), DqnV2_2i(822, 210), DqnV2_2i(846, 485),
	                 DqnV2_2i(348, 465), DqnV2_2i(722, 129), DqnV2_2i(365, 723), DqnV2_2i(847, 61),  DqnV2_2i(757, 206), DqnV2_2i(40, 630),  DqnV2_2i(777, 16),
	                 DqnV2_2i(495, 605), DqnV2_2i(218, 820), DqnV2_2i(87, 635),  DqnV2_2i(135, 293), DqnV2_2i(607, 733), DqnV2_2i(64, 531),  DqnV2_2i(220, 110),
	                 DqnV2_2i(704, 90),  DqnV2_2i(955, 103), DqnV2_2i(230, 72),  DqnV2_2i(172, 964), DqnV2_2i(194, 832), DqnV2_2i(481, 231), DqnV2_2i(201, 831),
	                 DqnV2_2i(337, 102), DqnV2_2i(680, 558), DqnV2_2i(235, 399), DqnV2_2i(580, 812), DqnV2_2i(25, 634),  DqnV2_2i(38, 587),  DqnV2_2i(45, 430),
	                 DqnV2_2i(589, 602), DqnV2_2i(726, 595), DqnV2_2i(388, 391), DqnV2_2i(502, 786), DqnV2_2i(719, 517), DqnV2_2i(395, 610), DqnV2_2i(67, 381),
	                 DqnV2_2i(650, 640), DqnV2_2i(640, 759), DqnV2_2i(506, 559), DqnV2_2i(550, 246), DqnV2_2i(275, 773), DqnV2_2i(256, 851), DqnV2_2i(326, 283),
	                 DqnV2_2i(84, 840),  DqnV2_2i(102, 975), DqnV2_2i(362, 139), DqnV2_2i(659, 101), DqnV2_2i(449, 706), DqnV2_2i(588, 403), DqnV2_2i(387, 470),
	                 DqnV2_2i(725, 27),  DqnV2_2i(24, 969),  DqnV2_2i(714, 658), DqnV2_2i(392, 828), DqnV2_2i(233, 705), DqnV2_2i(12, 73),   DqnV2_2i(704, 587),
	                 DqnV2_2i(180, 379), DqnV2_2i(274, 381), DqnV2_2i(98, 444),  DqnV2_2i(514, 65),  DqnV2_2i(676, 667), DqnV2_2i(642, 422), DqnV2_2i(24, 338),
	                 DqnV2_2i(663, 410), DqnV2_2i(253, 761), DqnV2_2i(499, 247), DqnV2_2i(946, 84),  DqnV2_2i(524, 9),   DqnV2_2i(433, 803), DqnV2_2i(520, 287),
	                 DqnV2_2i(903, 265), DqnV2_2i(768, 23),  DqnV2_2i(36, 883),  DqnV2_2i(547, 130), DqnV2_2i(88, 276),  DqnV2_2i(596, 388), DqnV2_2i(54, 283),
	                 DqnV2_2i(39, 468),  DqnV2_2i(248, 104), DqnV2_2i(17, 321),  DqnV2_2i(271, 48),  DqnV2_2i(618, 155), DqnV2_2i(612, 691), DqnV2_2i(40, 208),
	                 DqnV2_2i(930, 172), DqnV2_2i(298, 604), DqnV2_2i(389, 427), DqnV2_2i(815, 16),  DqnV2_2i(767, 444), DqnV2_2i(555, 433), DqnV2_2i(698, 587),
	                 DqnV2_2i(934, 327), DqnV2_2i(224, 412), DqnV2_2i(48, 905),  DqnV2_2i(559, 110), DqnV2_2i(192, 469), DqnV2_2i(157, 404), DqnV2_2i(76, 258),
	                 DqnV2_2i(219, 930), DqnV2_2i(643, 258), DqnV2_2i(97, 823),  DqnV2_2i(125, 640), DqnV2_2i(507, 671), DqnV2_2i(578, 741), DqnV2_2i(756, 621),
	                 DqnV2_2i(762, 544), DqnV2_2i(178, 150), DqnV2_2i(493, 764), DqnV2_2i(252, 157), DqnV2_2i(345, 579), DqnV2_2i(884, 445), DqnV2_2i(180, 863),
	                 DqnV2_2i(255, 307), DqnV2_2i(492, 677), DqnV2_2i(208, 126), DqnV2_2i(650, 150), DqnV2_2i(789, 52),  DqnV2_2i(642, 186), DqnV2_2i(707, 517),
	                 DqnV2_2i(881, 111), DqnV2_2i(10, 263),  DqnV2_2i(20, 630),  DqnV2_2i(593, 438), DqnV2_2i(34, 249),  DqnV2_2i(987, 151), DqnV2_2i(240, 151),
	                 DqnV2_2i(177, 843), DqnV2_2i(758, 133), DqnV2_2i(776, 607), DqnV2_2i(838, 129), DqnV2_2i(668, 5),   DqnV2_2i(8, 993),   DqnV2_2i(938, 242),
	                 DqnV2_2i(870, 373), DqnV2_2i(930, 301), DqnV2_2i(381, 591), DqnV2_2i(978, 92),  DqnV2_2i(399, 731), DqnV2_2i(176, 485), DqnV2_2i(84, 53),
	                 DqnV2_2i(2, 79),    DqnV2_2i(68, 638),  DqnV2_2i(563, 535), DqnV2_2i(705, 222), DqnV2_2i(312, 252), DqnV2_2i(764, 595), DqnV2_2i(925, 119),
	                 DqnV2_2i(555, 426), DqnV2_2i(112, 920), DqnV2_2i(672, 346), DqnV2_2i(756, 157), DqnV2_2i(44, 29),   DqnV2_2i(389, 591), DqnV2_2i(899, 140),
	                 DqnV2_2i(613, 761), DqnV2_2i(385, 547), DqnV2_2i(586, 650), DqnV2_2i(952, 138), DqnV2_2i(175, 723), DqnV2_2i(102, 542), DqnV2_2i(754, 394),
	                 DqnV2_2i(490, 456), DqnV2_2i(219, 483), DqnV2_2i(236, 83),  DqnV2_2i(400, 79),  DqnV2_2i(53, 311),  DqnV2_2i(523, 790), DqnV2_2i(71, 411),
	                 DqnV2_2i(632, 599), DqnV2_2i(103, 120), DqnV2_2i(378, 222), DqnV2_2i(583, 652), DqnV2_2i(731, 441), DqnV2_2i(713, 218), DqnV2_2i(468, 223),
	                 DqnV2_2i(440, 515), DqnV2_2i(28, 809),  DqnV2_2i(596, 746), DqnV2_2i(794, 153), DqnV2_2i(196, 920), DqnV2_2i(489, 336), DqnV2_2i(272, 465),
	                 DqnV2_2i(420, 412), DqnV2_2i(424, 758), DqnV2_2i(536, 796), DqnV2_2i(658, 632), DqnV2_2i(868, 135), DqnV2_2i(901, 266), DqnV2_2i(199, 690),
	                 DqnV2_2i(234, 592), DqnV2_2i(974, 126), DqnV2_2i(79, 113),  DqnV2_2i(763, 269), DqnV2_2i(655, 569), DqnV2_2i(179, 896), DqnV2_2i(509, 395),
	                 DqnV2_2i(402, 284), DqnV2_2i(656, 692), DqnV2_2i(904, 348), DqnV2_2i(309, 586), DqnV2_2i(60, 270),  DqnV2_2i(422, 899), DqnV2_2i(705, 86),
	                 DqnV2_2i(370, 614), DqnV2_2i(184, 137), DqnV2_2i(247, 135), DqnV2_2i(122, 338), DqnV2_2i(817, 118), DqnV2_2i(492, 622), DqnV2_2i(621, 763),
	                 DqnV2_2i(60, 464),  DqnV2_2i(370, 801), DqnV2_2i(86, 7),    DqnV2_2i(500, 342), DqnV2_2i(634, 592), DqnV2_2i(644, 749), DqnV2_2i(453, 687),
	                 DqnV2_2i(387, 137), DqnV2_2i(223, 823), DqnV2_2i(119, 85),  DqnV2_2i(347, 866), DqnV2_2i(930, 306), DqnV2_2i(71, 318),  DqnV2_2i(236, 282),
	                 DqnV2_2i(661, 55),  DqnV2_2i(300, 647), DqnV2_2i(923, 352), DqnV2_2i(787, 524), DqnV2_2i(221, 52),  DqnV2_2i(446, 134), DqnV2_2i(164, 218),
	                 DqnV2_2i(8, 146),   DqnV2_2i(212, 459), DqnV2_2i(836, 25),  DqnV2_2i(29, 882),  DqnV2_2i(377, 792), DqnV2_2i(305, 45),  DqnV2_2i(554, 313),
	                 DqnV2_2i(25, 51),   DqnV2_2i(153, 485), DqnV2_2i(598, 647), DqnV2_2i(137, 134), DqnV2_2i(663, 666), DqnV2_2i(251, 592), DqnV2_2i(776, 606),
	                 DqnV2_2i(326, 747), DqnV2_2i(579, 726), DqnV2_2i(398, 641), DqnV2_2i(117, 130), DqnV2_2i(294, 292), DqnV2_2i(608, 407), DqnV2_2i(193, 19),
	                 DqnV2_2i(55, 688),  DqnV2_2i(122, 960), DqnV2_2i(625, 194), DqnV2_2i(234, 43),  DqnV2_2i(332, 401), DqnV2_2i(2, 931),   DqnV2_2i(119, 186),
	                 DqnV2_2i(835, 522), DqnV2_2i(962, 94),  DqnV2_2i(554, 411), DqnV2_2i(440, 186), DqnV2_2i(506, 542), DqnV2_2i(29, 333),  DqnV2_2i(212, 350),
	                 DqnV2_2i(716, 672), DqnV2_2i(512, 55),  DqnV2_2i(37, 131),  DqnV2_2i(5, 891),   DqnV2_2i(340, 18),  DqnV2_2i(125, 751), DqnV2_2i(365, 805),
	                 DqnV2_2i(178, 198), DqnV2_2i(662, 522), DqnV2_2i(734, 466), DqnV2_2i(222, 549), DqnV2_2i(692, 589), DqnV2_2i(89, 409),  DqnV2_2i(436, 151),
	                 DqnV2_2i(162, 747), DqnV2_2i(153, 745), DqnV2_2i(331, 341), DqnV2_2i(610, 504), DqnV2_2i(530, 739), DqnV2_2i(91, 21),   DqnV2_2i(553, 256),
	                 DqnV2_2i(677, 512), DqnV2_2i(377, 446), DqnV2_2i(636, 652), DqnV2_2i(463, 397), DqnV2_2i(749, 467), DqnV2_2i(220, 154), DqnV2_2i(518, 106),
	                 DqnV2_2i(578, 769), DqnV2_2i(164, 472), DqnV2_2i(310, 255), DqnV2_2i(275, 330), DqnV2_2i(305, 211), DqnV2_2i(88, 52),   DqnV2_2i(511, 273),
	                 DqnV2_2i(389, 869), DqnV2_2i(703, 541), DqnV2_2i(107, 134), DqnV2_2i(492, 800), DqnV2_2i(433, 610), DqnV2_2i(504, 355), DqnV2_2i(788, 28),
	                 DqnV2_2i(370, 612), DqnV2_2i(765, 81),  DqnV2_2i(840, 90),  DqnV2_2i(299, 836), DqnV2_2i(319, 507), DqnV2_2i(769, 559), DqnV2_2i(601, 706),
	                 DqnV2_2i(62, 810),  DqnV2_2i(930, 64),  DqnV2_2i(560, 739), DqnV2_2i(342, 170), DqnV2_2i(16, 971),  DqnV2_2i(427, 649), DqnV2_2i(147, 351),
	                 DqnV2_2i(701, 118), DqnV2_2i(87, 666),  DqnV2_2i(457, 255), DqnV2_2i(562, 313), DqnV2_2i(857, 74),  DqnV2_2i(173, 584), DqnV2_2i(830, 240),
	                 DqnV2_2i(82, 961),  DqnV2_2i(333, 376), DqnV2_2i(782, 364), DqnV2_2i(150, 828), DqnV2_2i(281, 376), DqnV2_2i(142, 935), DqnV2_2i(549, 138),
	                 DqnV2_2i(882, 305), DqnV2_2i(207, 919), DqnV2_2i(603, 224), DqnV2_2i(608, 222), DqnV2_2i(395, 65),  DqnV2_2i(277, 728), DqnV2_2i(649, 659),
	                 DqnV2_2i(986, 28),  DqnV2_2i(352, 789), DqnV2_2i(506, 749), DqnV2_2i(849, 334), DqnV2_2i(541, 735), DqnV2_2i(806, 529), DqnV2_2i(236, 228),
	                 DqnV2_2i(16, 151),  DqnV2_2i(266, 32),  DqnV2_2i(30, 745),  DqnV2_2i(613, 287), DqnV2_2i(324, 436), DqnV2_2i(758, 436), DqnV2_2i(230, 247),
	                 DqnV2_2i(98, 911),  DqnV2_2i(487, 270), DqnV2_2i(277, 531), DqnV2_2i(940, 336), DqnV2_2i(129, 665), DqnV2_2i(907, 297), DqnV2_2i(572, 219),
	                 DqnV2_2i(202, 558), DqnV2_2i(888, 245), DqnV2_2i(599, 436), DqnV2_2i(467, 749), DqnV2_2i(83, 985),  DqnV2_2i(232, 739), DqnV2_2i(337, 331),
	                 DqnV2_2i(83, 142),  DqnV2_2i(263, 163), DqnV2_2i(834, 111), DqnV2_2i(733, 680), DqnV2_2i(368, 584), DqnV2_2i(199, 679), DqnV2_2i(480, 11),
	                 DqnV2_2i(251, 609), DqnV2_2i(678, 668), DqnV2_2i(279, 649), DqnV2_2i(416, 291), DqnV2_2i(415, 96),  DqnV2_2i(731, 18),  DqnV2_2i(100, 823),
	                 DqnV2_2i(514, 679), DqnV2_2i(216, 360), DqnV2_2i(425, 886), DqnV2_2i(389, 457), DqnV2_2i(233, 959), DqnV2_2i(444, 31),  DqnV2_2i(311, 741),
	                 DqnV2_2i(971, 139), DqnV2_2i(752, 604), DqnV2_2i(14, 912),  DqnV2_2i(469, 280), DqnV2_2i(324, 670), DqnV2_2i(619, 215), DqnV2_2i(398, 838),
	                 DqnV2_2i(152, 133), DqnV2_2i(552, 380), DqnV2_2i(328, 795), DqnV2_2i(725, 109), DqnV2_2i(204, 526), DqnV2_2i(346, 696), DqnV2_2i(637, 358),
	                 DqnV2_2i(932, 3),   DqnV2_2i(48, 534),  DqnV2_2i(509, 235), DqnV2_2i(863, 208), DqnV2_2i(149, 825), DqnV2_2i(335, 498), DqnV2_2i(894, 179),
	                 DqnV2_2i(740, 98),  DqnV2_2i(267, 739), DqnV2_2i(325, 527), DqnV2_2i(557, 508), DqnV2_2i(696, 486), DqnV2_2i(795, 543), DqnV2_2i(687, 529),
	                 DqnV2_2i(91, 946),  DqnV2_2i(225, 962), DqnV2_2i(422, 609), DqnV2_2i(216, 326), DqnV2_2i(372, 318), DqnV2_2i(76, 539),  DqnV2_2i(692, 524),
	                 DqnV2_2i(234, 296), DqnV2_2i(587, 425), DqnV2_2i(446, 688), DqnV2_2i(566, 784), DqnV2_2i(237, 382), DqnV2_2i(491, 559), DqnV2_2i(667, 708),
	                 DqnV2_2i(804, 336), DqnV2_2i(562, 650), DqnV2_2i(890, 186), DqnV2_2i(279, 409), DqnV2_2i(5, 215),   DqnV2_2i(112, 267), DqnV2_2i(132, 822),
	                 DqnV2_2i(114, 141), DqnV2_2i(699, 572), DqnV2_2i(688, 580), DqnV2_2i(52, 68),   DqnV2_2i(434, 13),  DqnV2_2i(521, 461), DqnV2_2i(291, 257),
	                 DqnV2_2i(532, 452), DqnV2_2i(379, 301), DqnV2_2i(650, 376), DqnV2_2i(300, 715), DqnV2_2i(355, 392), DqnV2_2i(3, 441),   DqnV2_2i(435, 336),
	                 DqnV2_2i(127, 568), DqnV2_2i(958, 191), DqnV2_2i(821, 393), DqnV2_2i(314, 439), DqnV2_2i(816, 574), DqnV2_2i(530, 584), DqnV2_2i(430, 464),
	                 DqnV2_2i(253, 768), DqnV2_2i(747, 272), DqnV2_2i(842, 16),  DqnV2_2i(30, 656),  DqnV2_2i(133, 36),  DqnV2_2i(411, 134), DqnV2_2i(609, 193),
	                 DqnV2_2i(353, 407), DqnV2_2i(478, 875), DqnV2_2i(195, 854), DqnV2_2i(18, 776),  DqnV2_2i(473, 391), DqnV2_2i(258, 921), DqnV2_2i(531, 294),
	                 DqnV2_2i(35, 298),  DqnV2_2i(163, 358), DqnV2_2i(803, 162), DqnV2_2i(207, 351), DqnV2_2i(483, 256), DqnV2_2i(450, 164), DqnV2_2i(590, 627),
	                 DqnV2_2i(342, 801), DqnV2_2i(743, 433), DqnV2_2i(146, 158), DqnV2_2i(567, 428), DqnV2_2i(731, 377), DqnV2_2i(267, 338), DqnV2_2i(35, 566),
	                 DqnV2_2i(596, 392), DqnV2_2i(756, 544), DqnV2_2i(904, 410), DqnV2_2i(524, 80),  DqnV2_2i(918, 46),  DqnV2_2i(373, 850), DqnV2_2i(211, 248),
	                 DqnV2_2i(202, 38),  DqnV2_2i(499, 592), DqnV2_2i(119, 872), DqnV2_2i(80, 123),  DqnV2_2i(254, 295), DqnV2_2i(127, 491), DqnV2_2i(837, 254),
	                 DqnV2_2i(874, 392), DqnV2_2i(342, 162), DqnV2_2i(702, 606), DqnV2_2i(522, 20),  DqnV2_2i(232, 480), DqnV2_2i(388, 32),  DqnV2_2i(635, 554),
	                 DqnV2_2i(141, 445), DqnV2_2i(429, 888), DqnV2_2i(248, 507), DqnV2_2i(347, 729), DqnV2_2i(1, 700),   DqnV2_2i(315, 116), DqnV2_2i(800, 79),
	                 DqnV2_2i(136, 140), DqnV2_2i(15, 271),  DqnV2_2i(488, 81),  DqnV2_2i(618, 371), DqnV2_2i(477, 719), DqnV2_2i(312, 695), DqnV2_2i(800, 394),
	                 DqnV2_2i(208, 548), DqnV2_2i(14, 766),  DqnV2_2i(275, 55),  DqnV2_2i(509, 238), DqnV2_2i(206, 606), DqnV2_2i(95, 501),  DqnV2_2i(584, 562),
	                 DqnV2_2i(138, 677), DqnV2_2i(728, 242), DqnV2_2i(139, 690), DqnV2_2i(26, 138),  DqnV2_2i(343, 323), DqnV2_2i(957, 254), DqnV2_2i(466, 230),
	                 DqnV2_2i(409, 565), DqnV2_2i(134, 342), DqnV2_2i(336, 263), DqnV2_2i(826, 191), DqnV2_2i(12, 637),  DqnV2_2i(256, 197), DqnV2_2i(175, 882),
	                 DqnV2_2i(316, 555), DqnV2_2i(326, 873), DqnV2_2i(729, 158), DqnV2_2i(135, 57),  DqnV2_2i(796, 89),  DqnV2_2i(80, 806),  DqnV2_2i(901, 179),
	                 DqnV2_2i(837, 513), DqnV2_2i(345, 585), DqnV2_2i(20, 338),  DqnV2_2i(195, 270), DqnV2_2i(361, 659), DqnV2_2i(54, 243),  DqnV2_2i(218, 809),
	                 DqnV2_2i(639, 357), DqnV2_2i(989, 86),  DqnV2_2i(719, 116), DqnV2_2i(117, 155), DqnV2_2i(181, 106), DqnV2_2i(556, 385), DqnV2_2i(18, 791),
	                 DqnV2_2i(281, 801), DqnV2_2i(538, 634), DqnV2_2i(352, 436), DqnV2_2i(111, 806), DqnV2_2i(85, 502),  DqnV2_2i(765, 316), DqnV2_2i(222, 197),
	                 DqnV2_2i(701, 459), DqnV2_2i(209, 920), DqnV2_2i(970, 127), DqnV2_2i(894, 181), DqnV2_2i(318, 428), DqnV2_2i(315, 940), DqnV2_2i(371, 198),
	                 DqnV2_2i(123, 518), DqnV2_2i(276, 877), DqnV2_2i(216, 970), DqnV2_2i(509, 97),  DqnV2_2i(179, 291), DqnV2_2i(508, 46),  DqnV2_2i(161, 38),
	                 DqnV2_2i(798, 551), DqnV2_2i(971, 19),  DqnV2_2i(213, 283), DqnV2_2i(733, 558), DqnV2_2i(75, 618),  DqnV2_2i(558, 200), DqnV2_2i(844, 132),
	                 DqnV2_2i(45, 878),  DqnV2_2i(332, 27),  DqnV2_2i(812, 159), DqnV2_2i(325, 610), DqnV2_2i(322, 589), DqnV2_2i(62, 91),   DqnV2_2i(218, 495),
	                 DqnV2_2i(334, 841), DqnV2_2i(215, 324), DqnV2_2i(519, 307), DqnV2_2i(587, 188), DqnV2_2i(943, 289), DqnV2_2i(597, 56),  DqnV2_2i(86, 577),
	                 DqnV2_2i(603, 679), DqnV2_2i(357, 887), DqnV2_2i(664, 262), DqnV2_2i(406, 10),  DqnV2_2i(66, 492),  DqnV2_2i(597, 152), DqnV2_2i(554, 552),
	                 DqnV2_2i(432, 711), DqnV2_2i(168, 831), DqnV2_2i(671, 707), DqnV2_2i(541, 771), DqnV2_2i(459, 10),  DqnV2_2i(147, 404), DqnV2_2i(146, 80),
	                 DqnV2_2i(442, 187), DqnV2_2i(51, 135),  DqnV2_2i(49, 574),  DqnV2_2i(530, 781), DqnV2_2i(735, 228), DqnV2_2i(441, 518), DqnV2_2i(792, 65),
	                 DqnV2_2i(238, 290), DqnV2_2i(334, 729), DqnV2_2i(241, 271), DqnV2_2i(820, 542), DqnV2_2i(465, 486), DqnV2_2i(855, 127), DqnV2_2i(252, 299),
	                 DqnV2_2i(371, 884), DqnV2_2i(551, 677), DqnV2_2i(593, 668), DqnV2_2i(295, 260), DqnV2_2i(46, 34),   DqnV2_2i(64, 104),  DqnV2_2i(801, 141),
	                 DqnV2_2i(227, 506), DqnV2_2i(143, 974), DqnV2_2i(424, 2),   DqnV2_2i(410, 180), DqnV2_2i(800, 51),  DqnV2_2i(259, 351), DqnV2_2i(499, 514),
	                 DqnV2_2i(44, 314),  DqnV2_2i(386, 266), DqnV2_2i(478, 417), DqnV2_2i(338, 116), DqnV2_2i(644, 229), DqnV2_2i(683, 699), DqnV2_2i(450, 380),
	                 DqnV2_2i(78, 467),  DqnV2_2i(342, 750), DqnV2_2i(124, 655), DqnV2_2i(171, 84),  DqnV2_2i(635, 588), DqnV2_2i(207, 453), DqnV2_2i(368, 180),
	                 DqnV2_2i(193, 701), DqnV2_2i(243, 473), DqnV2_2i(497, 638), DqnV2_2i(26, 240),  DqnV2_2i(52, 691),  DqnV2_2i(25, 558),  DqnV2_2i(319, 480),
	                 DqnV2_2i(167, 674), DqnV2_2i(256, 865), DqnV2_2i(35, 392),  DqnV2_2i(740, 177), DqnV2_2i(881, 177), DqnV2_2i(380, 76),  DqnV2_2i(229, 962),
	                 DqnV2_2i(58, 500),  DqnV2_2i(223, 356), DqnV2_2i(101, 825), DqnV2_2i(402, 730), DqnV2_2i(753, 408), DqnV2_2i(496, 221), DqnV2_2i(525, 281),
	                 DqnV2_2i(598, 231), DqnV2_2i(392, 848), DqnV2_2i(89, 236),  DqnV2_2i(966, 49),  DqnV2_2i(806, 22),  DqnV2_2i(152, 453), DqnV2_2i(260, 619),
	                 DqnV2_2i(274, 878), DqnV2_2i(192, 604), DqnV2_2i(359, 262), DqnV2_2i(350, 607), DqnV2_2i(185, 593), DqnV2_2i(321, 693), DqnV2_2i(93, 104),
	                 DqnV2_2i(372, 589), DqnV2_2i(827, 185), DqnV2_2i(201, 786), DqnV2_2i(409, 329), DqnV2_2i(574, 91),  DqnV2_2i(317, 305), DqnV2_2i(600, 591),
	                 DqnV2_2i(353, 810), DqnV2_2i(23, 842),  DqnV2_2i(121, 847), DqnV2_2i(489, 833), DqnV2_2i(896, 70),  DqnV2_2i(74, 982),  DqnV2_2i(144, 970),
	                 DqnV2_2i(505, 256), DqnV2_2i(321, 64),  DqnV2_2i(1, 483),   DqnV2_2i(217, 406), DqnV2_2i(715, 304), DqnV2_2i(544, 166), DqnV2_2i(510, 529),
	                 DqnV2_2i(384, 338), DqnV2_2i(874, 213), DqnV2_2i(206, 255), DqnV2_2i(735, 140), DqnV2_2i(911, 367), DqnV2_2i(116, 987), DqnV2_2i(516, 223),
	                 DqnV2_2i(679, 719), DqnV2_2i(710, 388), DqnV2_2i(324, 929), DqnV2_2i(421, 128), DqnV2_2i(225, 454), DqnV2_2i(324, 647), DqnV2_2i(423, 188),
	                 DqnV2_2i(711, 160), DqnV2_2i(643, 677), DqnV2_2i(94, 986),  DqnV2_2i(162, 366), DqnV2_2i(444, 614), DqnV2_2i(201, 244), DqnV2_2i(650, 217),
	                 DqnV2_2i(265, 446), DqnV2_2i(904, 277), DqnV2_2i(102, 805), DqnV2_2i(558, 320), DqnV2_2i(105, 873), DqnV2_2i(840, 132), DqnV2_2i(22, 325),
	                 DqnV2_2i(581, 584), DqnV2_2i(573, 131), DqnV2_2i(757, 509), DqnV2_2i(677, 615), DqnV2_2i(612, 146), DqnV2_2i(179, 860), DqnV2_2i(114, 280),
	                 DqnV2_2i(109, 565), DqnV2_2i(923, 343), DqnV2_2i(276, 158), DqnV2_2i(617, 254), DqnV2_2i(374, 207), DqnV2_2i(417, 71),  DqnV2_2i(806, 114),
	                 DqnV2_2i(583, 281), DqnV2_2i(356, 584), DqnV2_2i(891, 424), DqnV2_2i(257, 94),  DqnV2_2i(315, 373), DqnV2_2i(727, 265), DqnV2_2i(914, 316),
	                 DqnV2_2i(651, 462), DqnV2_2i(538, 342), DqnV2_2i(415, 670), DqnV2_2i(408, 292), DqnV2_2i(286, 820), DqnV2_2i(942, 265), DqnV2_2i(362, 268),
	                 DqnV2_2i(286, 728), DqnV2_2i(820, 520), DqnV2_2i(642, 642), DqnV2_2i(637, 671), DqnV2_2i(758, 27),  DqnV2_2i(575, 11),  DqnV2_2i(884, 397),
	                 DqnV2_2i(842, 42),  DqnV2_2i(713, 414), DqnV2_2i(541, 77),  DqnV2_2i(433, 637), DqnV2_2i(521, 297), DqnV2_2i(950, 9),   DqnV2_2i(713, 506),
	                 DqnV2_2i(575, 114), DqnV2_2i(608, 778), DqnV2_2i(666, 395), DqnV2_2i(847, 515), DqnV2_2i(763, 268), DqnV2_2i(648, 493), DqnV2_2i(459, 208),
	                 DqnV2_2i(175, 288), DqnV2_2i(148, 352), DqnV2_2i(457, 559), DqnV2_2i(848, 114), DqnV2_2i(588, 497), DqnV2_2i(10, 362),  DqnV2_2i(534, 717),
	                 DqnV2_2i(20, 490),  DqnV2_2i(391, 33),  DqnV2_2i(347, 111), DqnV2_2i(825, 343), DqnV2_2i(510, 708), DqnV2_2i(89, 545),  DqnV2_2i(629, 722),
	                 DqnV2_2i(116, 587), DqnV2_2i(397, 598), DqnV2_2i(841, 43),  DqnV2_2i(21, 161),  DqnV2_2i(457, 544), DqnV2_2i(78, 612),  DqnV2_2i(30, 621),
	                 DqnV2_2i(605, 709), DqnV2_2i(273, 45),  DqnV2_2i(839, 333), DqnV2_2i(165, 96),  DqnV2_2i(457, 402), DqnV2_2i(598, 246), DqnV2_2i(710, 531),
	                 DqnV2_2i(409, 24),  DqnV2_2i(326, 685), DqnV2_2i(163, 692), DqnV2_2i(569, 770), DqnV2_2i(685, 144), DqnV2_2i(531, 389), DqnV2_2i(202, 172),
	                 DqnV2_2i(835, 549), DqnV2_2i(919, 173), DqnV2_2i(221, 316), DqnV2_2i(101, 978), DqnV2_2i(128, 270), DqnV2_2i(537, 388), DqnV2_2i(525, 286),
	                 DqnV2_2i(756, 527), DqnV2_2i(142, 489), DqnV2_2i(691, 574), DqnV2_2i(493, 751), DqnV2_2i(313, 262), DqnV2_2i(271, 264), DqnV2_2i(654, 17),
	                 DqnV2_2i(483, 815), DqnV2_2i(35, 716),  DqnV2_2i(240, 815), DqnV2_2i(260, 157), DqnV2_2i(45, 33),   DqnV2_2i(164, 770), DqnV2_2i(533, 191),
	                 DqnV2_2i(517, 603), DqnV2_2i(736, 54),  DqnV2_2i(337, 244), DqnV2_2i(41, 237),  DqnV2_2i(125, 179), DqnV2_2i(132, 517), DqnV2_2i(492, 344),
	                 DqnV2_2i(198, 15),  DqnV2_2i(545, 378), DqnV2_2i(449, 656), DqnV2_2i(666, 456), DqnV2_2i(313, 101), DqnV2_2i(508, 305), DqnV2_2i(300, 188),
	                 DqnV2_2i(541, 727), DqnV2_2i(788, 420), DqnV2_2i(350, 884), DqnV2_2i(916, 33),  DqnV2_2i(696, 596), DqnV2_2i(363, 639), DqnV2_2i(286, 845),
	                 DqnV2_2i(333, 816), DqnV2_2i(459, 303), DqnV2_2i(702, 625), DqnV2_2i(576, 195), DqnV2_2i(451, 546), DqnV2_2i(732, 191), DqnV2_2i(815, 276),
	                 DqnV2_2i(618, 770), DqnV2_2i(428, 248), DqnV2_2i(123, 389), DqnV2_2i(656, 120), DqnV2_2i(287, 422), DqnV2_2i(12, 451),  DqnV2_2i(465, 569),
	                 DqnV2_2i(58, 811),  DqnV2_2i(622, 42),  DqnV2_2i(730, 569), DqnV2_2i(310, 345), DqnV2_2i(444, 751), DqnV2_2i(629, 407), DqnV2_2i(811, 153),
	                 DqnV2_2i(735, 213), DqnV2_2i(207, 708), DqnV2_2i(327, 627), DqnV2_2i(559, 671), DqnV2_2i(97, 137),  DqnV2_2i(36, 142),  DqnV2_2i(600, 523),
	                 DqnV2_2i(700, 278), DqnV2_2i(525, 498), DqnV2_2i(139, 55),  DqnV2_2i(594, 326), DqnV2_2i(380, 160), DqnV2_2i(847, 179), DqnV2_2i(95, 192),
	                 DqnV2_2i(63, 66),   DqnV2_2i(855, 310), DqnV2_2i(523, 421), DqnV2_2i(283, 926), DqnV2_2i(639, 2),   DqnV2_2i(795, 251), DqnV2_2i(341, 199),
	                 DqnV2_2i(218, 518), DqnV2_2i(133, 356), DqnV2_2i(21, 85),   DqnV2_2i(475, 848), DqnV2_2i(63, 207),  DqnV2_2i(702, 561), DqnV2_2i(113, 382),
	                 DqnV2_2i(762, 478), DqnV2_2i(175, 519), DqnV2_2i(496, 351), DqnV2_2i(506, 468), DqnV2_2i(268, 310), DqnV2_2i(163, 108), DqnV2_2i(626, 49),
	                 DqnV2_2i(289, 787), DqnV2_2i(824, 381), DqnV2_2i(764, 175), DqnV2_2i(234, 685), DqnV2_2i(596, 49),  DqnV2_2i(175, 808), DqnV2_2i(275, 134),
	                 DqnV2_2i(730, 525), DqnV2_2i(369, 442), DqnV2_2i(758, 216), DqnV2_2i(871, 264), DqnV2_2i(578, 214), DqnV2_2i(659, 479), DqnV2_2i(148, 234),
	                 DqnV2_2i(266, 540), DqnV2_2i(475, 480), DqnV2_2i(3, 315),   DqnV2_2i(864, 503), DqnV2_2i(76, 375),  DqnV2_2i(672, 453), DqnV2_2i(183, 837),
	                 DqnV2_2i(776, 244), DqnV2_2i(702, 456), DqnV2_2i(880, 222), DqnV2_2i(341, 656), DqnV2_2i(889, 147), DqnV2_2i(148, 448), DqnV2_2i(654, 28),
	                 DqnV2_2i(827, 539), DqnV2_2i(120, 72),  DqnV2_2i(417, 139), DqnV2_2i(506, 187), DqnV2_2i(11, 497),  DqnV2_2i(164, 563), DqnV2_2i(102, 744),
	                 DqnV2_2i(90, 342),  DqnV2_2i(276, 77),  DqnV2_2i(567, 537), DqnV2_2i(314, 521), DqnV2_2i(565, 599), DqnV2_2i(139, 568), DqnV2_2i(756, 394),
	                 DqnV2_2i(473, 830), DqnV2_2i(837, 138), DqnV2_2i(669, 28),  DqnV2_2i(463, 81),  DqnV2_2i(92, 832),  DqnV2_2i(575, 282), DqnV2_2i(829, 486),
	                 DqnV2_2i(482, 863), DqnV2_2i(329, 719), DqnV2_2i(94, 256),  DqnV2_2i(646, 91),  DqnV2_2i(614, 12),  DqnV2_2i(717, 38),  DqnV2_2i(629, 515),
	                 DqnV2_2i(590, 689), DqnV2_2i(74, 847),  DqnV2_2i(45, 634),  DqnV2_2i(291, 411), DqnV2_2i(138, 756), DqnV2_2i(514, 520), DqnV2_2i(470, 522),
	                 DqnV2_2i(442, 514), DqnV2_2i(700, 218), DqnV2_2i(294, 211), DqnV2_2i(273, 480), DqnV2_2i(591, 498), DqnV2_2i(141, 955), DqnV2_2i(651, 205),
	                 DqnV2_2i(353, 63),  DqnV2_2i(374, 175), DqnV2_2i(30, 82),   DqnV2_2i(396, 535), DqnV2_2i(73, 300),  DqnV2_2i(683, 25),  DqnV2_2i(156, 502),
	                 DqnV2_2i(165, 84),  DqnV2_2i(193, 154), DqnV2_2i(160, 592), DqnV2_2i(174, 929), DqnV2_2i(470, 117), DqnV2_2i(778, 312), DqnV2_2i(951, 173),
	                 DqnV2_2i(769, 23),  DqnV2_2i(314, 466), DqnV2_2i(718, 236), DqnV2_2i(699, 323), DqnV2_2i(158, 901), DqnV2_2i(735, 25),  DqnV2_2i(391, 496),
	                 DqnV2_2i(331, 349), DqnV2_2i(25, 350),  DqnV2_2i(459, 233), DqnV2_2i(888, 276), DqnV2_2i(418, 902), DqnV2_2i(124, 83),  DqnV2_2i(812, 261),
	                 DqnV2_2i(257, 616), DqnV2_2i(965, 141), DqnV2_2i(395, 774), DqnV2_2i(317, 867), DqnV2_2i(162, 605), DqnV2_2i(224, 95),  DqnV2_2i(213, 99),
	                 DqnV2_2i(192, 421), DqnV2_2i(305, 478), DqnV2_2i(849, 358), DqnV2_2i(435, 784), DqnV2_2i(467, 201), DqnV2_2i(57, 840),  DqnV2_2i(39, 387),
	                 DqnV2_2i(358, 570), DqnV2_2i(549, 656), DqnV2_2i(400, 282), DqnV2_2i(23, 184),  DqnV2_2i(360, 93),  DqnV2_2i(278, 749), DqnV2_2i(33, 472),
	                 DqnV2_2i(830, 76),  DqnV2_2i(743, 420), DqnV2_2i(363, 579), DqnV2_2i(570, 483), DqnV2_2i(962, 229), DqnV2_2i(373, 880), DqnV2_2i(853, 439),
	                 DqnV2_2i(32, 907),  DqnV2_2i(243, 332), DqnV2_2i(325, 627), DqnV2_2i(468, 380), DqnV2_2i(324, 634), DqnV2_2i(420, 668), DqnV2_2i(300, 684),
	                 DqnV2_2i(512, 345), DqnV2_2i(406, 374), DqnV2_2i(898, 84),  DqnV2_2i(693, 233), DqnV2_2i(353, 501), DqnV2_2i(40, 166),  DqnV2_2i(923, 149),
	                 DqnV2_2i(683, 26),  DqnV2_2i(57, 570),  DqnV2_2i(528, 455), DqnV2_2i(363, 793), DqnV2_2i(573, 397), DqnV2_2i(295, 846), DqnV2_2i(60, 656),
	                 DqnV2_2i(457, 674), DqnV2_2i(642, 499), DqnV2_2i(240, 202), DqnV2_2i(120, 363), DqnV2_2i(673, 202), DqnV2_2i(808, 143), DqnV2_2i(209, 105),
	                 DqnV2_2i(471, 848), DqnV2_2i(299, 133), DqnV2_2i(565, 810), DqnV2_2i(270, 710), DqnV2_2i(748, 286), DqnV2_2i(621, 150), DqnV2_2i(84, 61),
	                 DqnV2_2i(432, 550), DqnV2_2i(262, 489), DqnV2_2i(415, 763), DqnV2_2i(70, 527),  DqnV2_2i(309, 408), DqnV2_2i(404, 842), DqnV2_2i(471, 770),
	                 DqnV2_2i(279, 835), DqnV2_2i(340, 369), DqnV2_2i(298, 157), DqnV2_2i(344, 908), DqnV2_2i(838, 360), DqnV2_2i(939, 314), DqnV2_2i(1, 266),
	                 DqnV2_2i(189, 287), DqnV2_2i(613, 787), DqnV2_2i(134, 113), DqnV2_2i(463, 613), DqnV2_2i(450, 297), DqnV2_2i(354, 739), DqnV2_2i(583, 516),
	                 DqnV2_2i(69, 164),  DqnV2_2i(376, 273), DqnV2_2i(122, 732), DqnV2_2i(378, 766), DqnV2_2i(133, 195), DqnV2_2i(758, 528), DqnV2_2i(549, 132),
	                 DqnV2_2i(518, 522), DqnV2_2i(47, 319),  DqnV2_2i(452, 3),   DqnV2_2i(259, 136), DqnV2_2i(560, 360), DqnV2_2i(327, 693), DqnV2_2i(170, 344),
	                 DqnV2_2i(294, 886), DqnV2_2i(363, 205), DqnV2_2i(622, 44),  DqnV2_2i(35, 813),  DqnV2_2i(721, 507), DqnV2_2i(615, 230), DqnV2_2i(698, 515),
	                 DqnV2_2i(20, 888),  DqnV2_2i(746, 457), DqnV2_2i(586, 189), DqnV2_2i(347, 762), DqnV2_2i(126, 620), DqnV2_2i(237, 390), DqnV2_2i(31, 136),
	                 DqnV2_2i(421, 515), DqnV2_2i(554, 221), DqnV2_2i(97, 536),  DqnV2_2i(508, 684), DqnV2_2i(448, 364), DqnV2_2i(251, 6),   DqnV2_2i(830, 416),
	                 DqnV2_2i(846, 122), DqnV2_2i(821, 28)};
#endif

	i32 startIndex = 0;
	i32 endIndex   = DQN_ARRAY_COUNT(pList);
#if 1
	// Bubble sort Y descending
	{
		i32 iterateSize = endIndex - 1;
		for (bool hasSwapped = true; hasSwapped;)
		{
			hasSwapped = false;
			iterateSize--;
			for (i32 i = startIndex; i < iterateSize; i++)
			{
				DqnV2 a = pList[i];
				DqnV2 b = pList[i + 1];
				if (a.y < b.y)
				{
					DQN_SWAP(DqnV2, pList[i], pList[i + 1]);
					hasSwapped = true;
				}
			}
		}
	}
#else
	struct {
		bool operator()(DqnV2 a, DqnV2 b) const { return (a.y > b.y); }
	} DqnV2Sort;
	std::sort(pList, &pList[DQN_ARRAY_COUNT(pList)-1], DqnV2Sort);
#endif

	// Take the largest Y as the init point of the skyline, then "trash" it from the array
	i32 skylineIterations = 1;
	i32 skyPIndex         = 0;
	DqnV2 skyP[256]       = {};
	for (i32 sIterations = 0; sIterations < skylineIterations; sIterations++)
	{
		skyPIndex = 0;
		skyP[skyPIndex++]   = pList[startIndex];
		pList[startIndex++] = DqnV2_2f(-999, -999);

		for (;;)
		{
			DQN_ASSERT(skyPIndex - 1 >= 0);
			DqnV2 lastSkyP = skyP[skyPIndex - 1];

			i32 candidateIndex = -1;
			DqnV2 currSkyP     = DqnV2_2f(1000, -100);
			for (i32 i = startIndex; i < endIndex; i++)
			{
				DqnV2 checkP = pList[i];
				if (checkP.x == -999 && checkP.y == -999) continue;

				if (checkP.y > currSkyP.y && checkP.x > lastSkyP.x)
				{
					currSkyP       = checkP;
					candidateIndex = i;
				}
			}

			if (candidateIndex != -1)
			{
				DQN_ASSERT(candidateIndex >= startIndex && candidateIndex < endIndex);
				DQN_ASSERT(skyPIndex + 1 < DQN_ARRAY_COUNT(skyP));
				pList[candidateIndex] = DqnV2_2f(-999, -999);
				skyP[skyPIndex++]     = currSkyP;
			}
			else
			{
				break;
			}
		}
	}

	DTRState *state = (DTRState *)memory->context;
	DqnV4 textColor = DqnV4_4f(255, 255, 255, 255);
	DqnV2 radius    = DqnV2_1f(2);

	DqnV2 shiftP = DqnV2_2f(0, 30);
	f32 expandP  = 0.9f;
	for (i32 i = 0; i < DQN_ARRAY_COUNT(pList); i++)
	{
		DqnV4 pColor  = DqnV4_4f(255, 0, 255, 255);
		DqnV2 origP   = pList[i];
		DqnV2 p       = (origP + shiftP) * expandP;

#if 1
		char pText[32] = {};
		Dqn_sprintf(pText, "(%1.0f, %1.0f)", origP.x, origP.y);
		DTRRender_Text(renderBuffer, state->font,
		               DqnV2_2f(p.x + radius.x + 5, p.y - (state->font.sizeInPt * 0.40f)), pText,
		               textColor);
#endif
		DTRRender_Rectangle(renderBuffer, p - radius, p + radius, pColor);
	}

	DqnV2 halfRadius = radius * 0.5f;
	for (i32 i = 0; i < skyPIndex; i++)
	{
		DqnV4 pColor  = DqnV4_4f(0, 255, 255, 255);
		DqnV2 origP   = skyP[i];
		DqnV2 p       = (origP + shiftP) * expandP;

		char pText[32] = {};
		Dqn_sprintf(pText, "(%1.0f, %1.0f)", origP.x, origP.y);
		DTRRender_Text(renderBuffer, state->font,
		               DqnV2_2f(p.x + radius.x + 5, p.y - (state->font.sizeInPt * 0.40f)), pText,
		               textColor);
		DTRRender_Rectangle(renderBuffer, p - radius, p + radius, pColor);

		if (i + 1 <= skyPIndex && i > 0)
		{
			DqnV2 prevOrigP = skyP[i - 1];
			DqnV2 prevP     = (prevOrigP + shiftP) * expandP;

			DqnV2 pMid     = p + halfRadius;
			DqnV2 prevPMid = prevP + halfRadius;
			DTRRender_Line(renderBuffer, DqnV2i_V2(prevPMid), DqnV2i_V2(pMid),
			               DqnV4_4f(255, 0, 0, 255));
		}
	}
}

FILE_SCOPE void TestStrToF32Converter()
{
	const f32 EPSILON = 0.001f;
	const char a[]    = "-0.66248";
	f32 vA            = Dqn_StrToF32(a, DQN_ARRAY_COUNT(a));
	DQN_ASSERT(DQN_ABS(vA) - DQN_ABS(-0.66248f) < EPSILON);

	const char b[] = "-0.632053";
	f32 vB         = Dqn_StrToF32(b, DQN_ARRAY_COUNT(b));
	DQN_ASSERT(DQN_ABS(vB) - DQN_ABS(-0.632053f) < EPSILON);

	const char c[] = "-0.244271";
	f32 vC         = Dqn_StrToF32(c, DQN_ARRAY_COUNT(c));
	DQN_ASSERT(DQN_ABS(vC) - DQN_ABS(-0.244271f) < EPSILON);

	const char d[] = "-0.511812";
	f32 vD         = Dqn_StrToF32(d, DQN_ARRAY_COUNT(d));
	DQN_ASSERT(DQN_ABS(vD) - DQN_ABS(-0.511812f) < EPSILON);

	const char e[] = "-0.845392";
	f32 vE         = Dqn_StrToF32(e, DQN_ARRAY_COUNT(e));
	DQN_ASSERT(DQN_ABS(vE) - DQN_ABS(-0.845392f) < EPSILON);

	const char f[] = "0.127809";
	f32 vF         = Dqn_StrToF32(f, DQN_ARRAY_COUNT(f));
	DQN_ASSERT(DQN_ABS(vF) - DQN_ABS(-0.127809f) < EPSILON);

	const char g[] = "0.532";
	f32 vG         = Dqn_StrToF32(g, DQN_ARRAY_COUNT(g));
	DQN_ASSERT(DQN_ABS(vG) - DQN_ABS(-0.532f) < EPSILON);

	const char h[] = "0.923";
	f32 vH         = Dqn_StrToF32(h, DQN_ARRAY_COUNT(h));
	DQN_ASSERT(DQN_ABS(vH) - DQN_ABS(-0.923f) < EPSILON);

	const char i[] = "0.000";
	f32 vI         = Dqn_StrToF32(i, DQN_ARRAY_COUNT(i));
	DQN_ASSERT(DQN_ABS(vI) - DQN_ABS(-0.000f) < EPSILON);

	const char j[] = "0.000283538";
	f32 vJ         = Dqn_StrToF32(j, DQN_ARRAY_COUNT(j));
	DQN_ASSERT(DQN_ABS(vJ) - DQN_ABS(-0.000283538f) < EPSILON);

	const char k[] = "-1.25";
	f32 vK         = Dqn_StrToF32(k, DQN_ARRAY_COUNT(k));
	DQN_ASSERT(DQN_ABS(vK) - DQN_ABS(-1.25f) < EPSILON);

	const char l[] = "0.286843";
	f32 vL         = Dqn_StrToF32(l, DQN_ARRAY_COUNT(l));
	DQN_ASSERT(DQN_ABS(vL) - DQN_ABS(-0.286843f) < EPSILON);

	const char m[] = "-0.406";
	f32 vM         = Dqn_StrToF32(m, DQN_ARRAY_COUNT(m));
	DQN_ASSERT(DQN_ABS(vM) - DQN_ABS(-0.406f) < EPSILON);

	const char n[] = "-0.892";
	f32 vN         = Dqn_StrToF32(n, DQN_ARRAY_COUNT(n));
	DQN_ASSERT(DQN_ABS(vN) - DQN_ABS(-0.892f) < EPSILON);

	const char o[] = "0.201";
	f32 vO         = Dqn_StrToF32(o, DQN_ARRAY_COUNT(o));
	DQN_ASSERT(DQN_ABS(vO) - DQN_ABS(-0.201f) < EPSILON);

	const char p[] = "1.25";
	f32 vP         = Dqn_StrToF32(p, DQN_ARRAY_COUNT(p));
	DQN_ASSERT(DQN_ABS(vP) - DQN_ABS(1.25f) < EPSILON);

	const char q[] = "9.64635e-05";
	f32 vQ         = Dqn_StrToF32(q, DQN_ARRAY_COUNT(q));
	DQN_ASSERT(DQN_ABS(vQ) - DQN_ABS(9.64635e-05) < EPSILON);
}

extern "C" void DTR_Update(PlatformRenderBuffer *const renderBuffer,
                           PlatformInput *const input,
                           PlatformMemory *const memory)
{
	DTRState *state = (DTRState *)memory->context;
	if (input->executableReloaded)
	{
		DTR_DEBUG_PROFILE_END();
		DTR_DEBUG_PROFILE_START();
	}

	DTR_DEBUG_TIMED_FUNCTION();
	if (!memory->isInit)
	{
		TestStrToF32Converter();
		DTR_DEBUG_TIMED_BLOCK("DTR_Update Memory Initialisation");
		// NOTE(doyle): Do premultiply ourselves
		stbi_set_unpremultiply_on_load(true);
		stbi_set_flip_vertically_on_load(true);

		memory->isInit = true;
		memory->context =
		    DqnMemBuffer_Allocate(&memory->permanentBuffer, sizeof(DTRState));
		DQN_ASSERT(memory->context);

		state = (DTRState *)memory->context;
		BitmapFontCreate(input->api, memory, &state->font, "Roboto-bold.ttf",
		                 DqnV2i_2i(256, 256), DqnV2i_2i(' ', '~'), 12);
		BitmapLoad(input->api, &state->bitmap, "tree00.bmp",
		           &memory->transientBuffer);

		DTRBitmap test = {};
		DqnTempBuffer tmp = DqnMemBuffer_BeginTempRegion(&memory->permanentBuffer);
		BitmapLoad(input->api, &test, "byte_read_check.bmp",
		           &memory->transientBuffer);
		int x = 5;
		DqnMemBuffer_EndTempRegion(tmp);

		ObjWavefrontLoad(input->api, memory, "african_head.obj");
	}
	DTRRender_Clear(renderBuffer, DqnV3_3f(0, 0, 0));

#if 1
	DqnV4 colorRed = DqnV4_4f(0.8f, 0, 0, 1);
	DqnV2i bufferMidP =
	    DqnV2i_2f(renderBuffer->width * 0.5f, renderBuffer->height * 0.5f);
	i32 boundsOffset = 100;

	DqnV2 t0[3] = {DqnV2_2i(10, 70), DqnV2_2i(50, 160), DqnV2_2i(70, 80)};
	DqnV2 t1[3] = {DqnV2_2i(180, 50),  DqnV2_2i(150, 1),   DqnV2_2i(70, 180)};
	DqnV2 t2[3] = {DqnV2_2i(180, 150), DqnV2_2i(120, 160), DqnV2_2i(130, 180)};
	LOCAL_PERSIST DqnV2 t3[3] = {
	    DqnV2_2i(boundsOffset, boundsOffset),
	    DqnV2_2i(bufferMidP.w, renderBuffer->height - boundsOffset),
	    DqnV2_2i(renderBuffer->width - boundsOffset, boundsOffset)};

	DTRRender_Triangle(renderBuffer, t0[0], t0[1], t0[2], colorRed);
	DTRRender_Triangle(renderBuffer, t1[0], t1[1], t1[2], colorRed);
	DTRRender_Triangle(renderBuffer, t2[0], t2[1], t2[2], colorRed);

	DqnV4 colorRedHalfA        = DqnV4_4f(1, 0, 0, 0.1f);
	LOCAL_PERSIST f32 rotation = 0;
	rotation += input->deltaForFrame * 0.25f;

#if 1
	DTRRenderTransform defaultTransform = DTRRender_DefaultTransform();
	defaultTransform.rotation           = rotation + 45;
	DTRRender_Rectangle(renderBuffer, DqnV2_1f(300.0f), DqnV2_1f(300 + 100.0f), DqnV4_4f(0, 1.0f, 1.0f, 1.0f),
	                    defaultTransform);
#endif

	// Rotating triangle
	{
		DTRRenderTransform triTransform = DTRRender_DefaultTriangleTransform();
		triTransform.rotation           = rotation;
		DTRRender_Triangle(renderBuffer, t3[0], t3[1], t3[2], colorRedHalfA, triTransform);
	}


	DqnV2 fontP = DqnV2_2i(200, 180);
	DTRRender_Text(renderBuffer, state->font, fontP, "hello world!", DqnV4_4f(0, 0, 0, 1));

	DTRRenderTransform transform = DTRRender_DefaultTransform();
	transform.rotation           = 0;
	transform.scale              = DqnV2_1f(2.0f);

	LOCAL_PERSIST DqnV2 bitmapP = DqnV2_2f(300, 250);
	bitmapP.x += 3.0f * sinf((f32)input->timeNowInS * 0.5f);

	f32 cAngle = (f32)input->timeNowInS;
	DqnV4 color = DqnV4_4f(0.5f + 0.5f * sinf(cAngle), 0.5f + 0.5f * sinf(2.9f * cAngle),
	                       0.5f + 0.5f * cosf(10.0f * cAngle), 1.0f);
	DTRRender_Bitmap(renderBuffer, &state->bitmap, bitmapP, transform, color);

#else
	CompAssignment(renderBuffer, input, memory);
#endif
	DTRDebug_Update(state, renderBuffer, input, memory);
}
