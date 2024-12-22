#include "nlohmann/json.hpp"

#include "cute_png.h"

#include "ChatgptFunctions.h"

#include <direct.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

//There's old junk in here, the meaning is not clear or important for most of these.
struct LetterRecord
{
	int c;
	int x, y, width, height;
	int logw;
	int xo, yo;
};

struct FizzyFontKeyData
{
	std::string face;
	int size;
	double outline = 0;

	std::string GenerateKeyString()
	{
		char tmp[1000];
		sprintf(tmp,"%s_px%d_o%g",face.c_str(),size,outline);
		return tmp;
	}
};

struct State
{
	FizzyFontKeyData key;
	std::string input;
};

class ImageServer
{
public:

	std::string InputDirectory;

	cp_image_t* GetImage(const std::string& path)
	{
		auto it = images.find(path);
		if(it != images.end())
			return it->second;

		std::string fullpath = InputDirectory + "/" + path + ".png";

		cp_image_t* img = new cp_image_t();
		*img = cp_load_png(fullpath.c_str());
		images[path] = img;
		return img;
	}

	~ImageServer()
	{
		for(auto it : images)
		{
			cp_free_png(it.second);
			delete it.second;
		}
	}

private:
	std::unordered_map<std::string, cp_image_t*> images;
};

static ImageServer imageServer;

struct Job
{
	State state;

	struct 
	{
		int imageWidth, imageHeight;
	} inputInfo;

	struct
	{
		std::string path;
		nlohmann::json json;
	} outputInfo;

	std::vector<LetterRecord> letterRecords;
	
	void PreProcess()
	{
		//Generate a key string and use it to create the output path
		std::string keystr = state.key.GenerateKeyString();
		outputInfo.path = keystr + ".png";

		//--------------------
		//Reading input format

		cp_image_t *img = imageServer.GetImage(state.input);

		const int cellPadding = 1;

		int curChar = 0;
		int imageWidth = inputInfo.imageWidth = img->w;
		int imageHeight = inputInfo.imageHeight = img->h;

		uint8_t curAlpha = img->pix[0].a;
		int lastx = 0;
		curChar = ' ';
		for (int x = 1; x < imageWidth; x++)
		{
			uint8_t a = img->pix[x].a;
			if(a == curAlpha) continue;
			else
			{
				int w = x - lastx;
				auto lr = LetterRecord{curChar, lastx, 1, w, imageHeight-1, w, 0, 0};
				letterRecords.push_back(lr);
				lastx = x;
				curAlpha = img->pix[x].a;
				curChar++;
			}
		}
		//huh? I guess it's the last letter
		{
			int w = imageWidth - lastx;
			auto lr = LetterRecord{curChar, lastx, 1, w, imageHeight-1, w, 0, 0};
			letterRecords.push_back(lr);
		}
	}

	void Process()
	{
		std::string keystr = state.key.GenerateKeyString();

		cp_image_t *img = imageServer.GetImage(state.input);

		//roughly approximate size of atlas. we'll over-allocate in case we need it
		//pessimistically estimate width by assuming all characters are the widest
		//add some padding at the same time
		int wmax = -1, hmax = -1;
		for(const auto &LR : letterRecords)
			wmax = std::max(wmax,LR.width), hmax = std::max(hmax,LR.height);
		const int kPadding = 1;
		wmax += kPadding;
		hmax += kPadding;
		const int nGuessedPixels = wmax * (int)letterRecords.size() * inputInfo.imageHeight;

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
					atlasPixels[(aty+gy)*w+atx+gx] = img->pix[(gy+lr.y)*img->w+gx+lr.x];
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

		//expand the height to make room for the final characters
		aty += kPadding + hmax;

		//prep a cute image to dump out
		cp_image_t atlasCpImage;
		atlasCpImage.pix = atlasPixels;
		atlasCpImage.w = w;
		atlasCpImage.h = (aty + 7) & ~7;
		cp_save_png(outputInfo.path.c_str(),&atlasCpImage);
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
		outputInfo.json["letters"] = jLetters;
		outputInfo.json["keyStr"] = keystr;
	}
};

int main(int argc, char* argv[])
{
	std::vector<std::thread> threads;

	const char* inputDir = argv[1];
	const char* outputDir = argv[2];

	//Change to input directory to simplify logic
	(void)_chdir(inputDir);
	imageServer.InputDirectory = inputDir;

	//read input file and split to lines
	FILE* inf = fopen("fizzyfont.txt","rb");
	fseek(inf,0,SEEK_END);
	long len = ftell(inf);
	fseek(inf,0,SEEK_SET);
	std::string data(len,0);
	fread((char*)data.data(),1,len,inf);
	fclose(inf);
	auto lines = splitLines(data);

	State state;
	std::vector<Job> jobs;

	//process commands on each line to create jobs
	for(const auto &_line : lines)
	{
		auto line = trim(_line);
		if(line.empty())
			continue;
		auto parts = splitBySpace(line);
		auto cmd = parts[0];
		if(cmd == "input")
			state.input = parts[1];
		else if(cmd == "outline")
			state.key.outline = atof(parts[1].c_str());
		else if(cmd == "key_face")
			state.key.face = parts[1];
		else if(cmd == "key_size")
			state.key.size = atoi(parts[1].c_str());
		else if(cmd == "gen")
		{
			Job j;
			j.state = state;
			jobs.push_back(j);
		}
	}

	printf("%d fonts\n",(int)jobs.size());

	//process input for each job
	//This is done in a separate pass so later I can change it to use keys to reduce the required amount of processing,
	//and then kick off the outputting
	parallelExecute(jobs,[](Job& J) { J.PreProcess(); });

	//change to output directory
	(void)_chdir(outputDir);

	//Do final processing and png output
	parallelExecute(jobs,[](Job& J) { J.Process(); });

	//combine all json into a single doc
	//build list of key strings as we go and do last minute check for dupes
	nlohmann::json jDatabase = nlohmann::json::array_t();
	std::unordered_set<std::string> keystrs;
	for(auto &J :jobs)
	{
		std::string keystr = J.state.key.GenerateKeyString();
		if(keystrs.find(keystr) != keystrs.end())
		{
			printf("DUPLICATE KEYSTR: %s", keystr.c_str());
			return -1;
		}
		keystrs.insert(keystr);
		jDatabase.push_back(J.outputInfo.json);
	}

	//dump the json
	FILE* jsonFile = fopen("fizzyfonts.json", "wb");
	fprintf(jsonFile, "%s\n", jDatabase.dump(1,'\t').c_str());
	fclose(jsonFile);

	return 0;
}
