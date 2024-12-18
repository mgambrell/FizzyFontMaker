#include "nlohmann/json.hpp"

#include "cute_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>

//There's old junk in here, the meaning is not clear or important for most of these.
struct LetterRecord
{
	int c;
	int x, y, width, height;
	int logw;
	int xo, yo;
};

void makeFont(const char* inputImage, const char* outputJson, const char* outputPng)
{
	cp_image_t img = cp_load_png(inputImage);

	std::vector<LetterRecord> letterRecords;

	const int cellPadding = 1;

	int curChar = 0;
	int imageWidth = img.w;
	int imageHeight = img.h;

	uint8_t curAlpha = img.pix[0].a;
	int lastx = 0;
	curChar = ' ';
	for (int x = 1; x < imageWidth; x++)
	{
		uint8_t a = img.pix[x].a;
		if(a == curAlpha) continue;
		else
		{
			int w = x - lastx;
			auto lr = LetterRecord{curChar, lastx, 1, w, imageHeight-1, w, 0, 0};
			letterRecords.push_back(lr);
			lastx = x;
			curAlpha = img.pix[x].a;
			curChar++;
		}
	}
	//huh? I guess it's the last letter
	{
		int w = imageWidth - lastx;
		auto lr = LetterRecord{curChar, lastx, 1, w, imageHeight-1, w, 0, 0};
		letterRecords.push_back(lr);
	}

	//roughly approximate size of atlas. we'll over-allocate in case we need it
	//pessimistically estimate width by assuming all characters are the widest
	//add some padding at the same time
	int wmax = -1, hmax = -1;
	for(const auto &LR : letterRecords)
		wmax = std::max(wmax,LR.width), hmax = std::max(hmax,LR.height);
	const int kPadding = 1;
	wmax += kPadding;
	hmax += kPadding;
	const int nGuessedPixels = wmax * letterRecords.size() * imageHeight;

	//Search each power of 2 width to find something that makes room
	//We start the height off bigger because we know we're going to multiply it by 2 later for a safety margin anyway
	int w = 1;
	int h = 2;
	for(;;)
	{
		if(w*h>=nGuessedPixels)
			break;
		w *= 2;
		h *= 2;
	}

	//allocate output buffer to transfer glyphs
	//double-size the height (pessimistically, since we've just been estimating)
	cp_pixel_t* atlasPixels = new cp_pixel_t[w*h*2];
	memset(atlasPixels,0,sizeof(cp_pixel_t)*w*h*2);

	//transfer glyphs to atlas, and update x/y in database to the atlas location as we go
	int atx = kPadding;
	int aty = kPadding;
	int idx=0;
	for (auto& lr : letterRecords)
	{
		//make sure we have horizontal room
		if(atx + lr.width > w)
		{
			atx = kPadding;
			aty += kPadding + hmax;
		}

		//(actually transfer glyphs)
		for(int gy=0;gy<lr.height;gy++)
		{
			for(int gx=0;gx<lr.width;gx++)
			{
				atlasPixels[(aty+gy)*w+atx+gx] = img.pix[(gy+lr.y)*img.w+gx+lr.x];
			}
		}

		//store position of glyph in atlas
		lr.x = atx;
		lr.y = aty;

		//advance by the wmax, not the letter size (this way, we keep things in a nicer grid)
		//atx += kPadding + wmax;
		//NAH:
		atx += kPadding + lr.width;
	}

	//done with input file
	cp_free_png(&img);

	//expand the height to make room for the final characters
	aty += kPadding + hmax;

	//prep a cute image to dump out
	cp_image_t atlasCpImage;
	atlasCpImage.pix = atlasPixels;
	atlasCpImage.w = w;
	atlasCpImage.h = (aty + 7) & ~7;
	cp_save_png(outputPng,&atlasCpImage);
	delete[] atlasPixels;

	//shove all letters in a json array
	nlohmann::json::array_t jLetters;
	for (const auto& lr : letterRecords)
	{
		jLetters.push_back(
			{
			{"char", lr.c},
			{"x", lr.x},
			{"y", lr.y},
			{"width", lr.width},
			{"height", lr.height},
			{"logw", lr.logw},
			{"xo", lr.xo},
			{"yo", lr.yo}
		});
	}

	//construct final json doc
	nlohmann::json jsonData;
	jsonData["letters"] = jLetters;

	FILE* jsonFile = fopen(outputJson, "wb");
	fprintf(jsonFile, "%s\n", jsonData.dump(1,'\t').c_str());
	fclose(jsonFile);
}

int main(int argc, char* argv[]) {
	if (argc != 5) {
		printf("Usage: fontmaker fizzfont <input_image> <output_json> <output_png>\n");
		return -1;
	}

	const char* inputImage = argv[2];
	const char* outputJson = argv[3];
	const char* outputPng = argv[4];

	makeFont(inputImage, outputJson, outputPng);

	return 0;
}
