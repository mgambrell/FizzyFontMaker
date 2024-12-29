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

#include "ltalloc.hpp"

//Wrapper for cute_png image
//There's not much need to have it as a wrapper, but that's what we've done.
struct CPImage : public cp_image_t
{
	CPImage(int width, int height)
	{
		this->w = width;
		this->h = height;
		pix = new cp_pixel_t[width*height];
		memset(pix,0,sizeof(cp_pixel_t)*width*height);
	}

	cp_pixel_t& at(int x, int y)
	{
		assert(x>=0&&x<w);
		assert(y>=0&&y<h);
		return pix[y*w+x];
	}

	virtual ~CPImage()
	{
		delete[] pix;
	}
};

//Encapsulates an isolated glyph. Carries its own bitmap data with it.
class Glyph : public CPImage
{
public:
	int c;
	int xo = 0, yo = 0;
	int atlas_x, atlas_y;

	Glyph(int width, int height)
		: CPImage(width, height)
	{
	}

	Glyph* CreateOffset(int offset_x, int offset_y, int new_width, int new_height)
	{
		Glyph* ret = new Glyph(new_width, new_height);
		ret->c = this->c;
		ret->xo = this->xo - offset_x;
		ret->yo = this->yo - offset_y;

		//if needed to clone it
		#if 0
		for(int y=0;y<h;y++)
			for(int x=0;x<w;x++)
				ret->at(x+offset_x,y+offset_y) = at(x,y);
		#endif

		return ret;
	}
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

	std::vector<Glyph*> glyphs;
	std::vector<Glyph*> outlineGlyphs;

	void PreProcess()
	{
		//Generate a key string and use it to create the output path
		std::string keystr = state.key.GenerateKeyString();
		outputInfo.path = keystr + ".png";

		//--------------------
		//Reading input format

		struct LetterRecord
		{
			int c;
			int x, y, width, height;
			int logw;
			int xo, yo;
		};

		std::vector<LetterRecord> letterRecords;

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

		//extract all to NewGlyphs
		for (auto& lr : letterRecords)
		{
			Glyph* g = new Glyph(lr.width, lr.height);
			g->c = lr.c;
			glyphs.push_back(g);
			
			for(int gy=0;gy<lr.height;gy++)
			{
				for(int gx=0;gx<lr.width;gx++)
				{
					g->at(gx,gy) = img->pix[(gy+lr.y)*img->w+gx+lr.x];
				}
			}
		}

		createOutlines();
	}

	void createOutlines()
	{
		int thickness = 2;

		//create a symmetric circular kernel with anti-aliasing and normalization
		int kernelWidth = thickness * 2 + 1;
		float *kernel = new float[kernelWidth * kernelWidth];
		float center = (float)thickness;
		float maxKernelValue = thickness + 0.5f;
		for (int y = 0; y < kernelWidth; y++) {
			for (int x = 0; x < kernelWidth; x++) {
				float dx = x - center;
				float dy = y - center;
				float distance = sqrt(dx * dx + dy * dy);
				float val = std::max(0.0f, maxKernelValue - distance);
				kernel[y * kernelWidth + x] = val / maxKernelValue;
			}
		}

		for (int i = 0; i < glyphs.size(); i++)
		{
			Glyph *rawGlyph = glyphs[i];

			const int rg_width = rawGlyph->w;
			const int rg_height = rawGlyph->h;
			const int og_width = rg_width + thickness * 2;
			const int og_height = rg_height + thickness * 2;

			Glyph *og = rawGlyph->CreateOffset(thickness, thickness, og_width, og_height);
			outlineGlyphs.push_back(og);

			//for each pixel in output:
			for (int y = 0; y < og_height; y++)
			{
				for (int x = 0; x < og_width; x++)
				{
					//calculate the kernel centered at the output pixel:
					float accumAlpha = 0.0f;
					for (int ky = 0; ky < kernelWidth; ky++)
					{
						for (int kx = 0; kx < kernelWidth; kx++)
						{
							int sampleX = x + kx - thickness;
							int sampleY = y + ky - thickness;

							//adjust sample coordinates relative to the input glyph
							int inputX = sampleX - thickness;
							int inputY = sampleY - thickness;

							//read alpha value from the input glyph or assume 0 if out of bounds
							uint8_t sampleAlpha = 0;
							if (inputX >= 0 && inputX < rg_width && inputY >= 0 && inputY < rg_height) {
								sampleAlpha = rawGlyph->at(inputX, inputY).a;
							}

							float kernelValue = kernel[ky * kernelWidth + kx];
							accumAlpha += sampleAlpha * kernelValue;
						}
					}

					//clamp the accumulated value and write it directly to the output glyph
					og->at(x, y).a = std::min(255, static_cast<int>(accumAlpha));
				}
			}
		}

		delete[] kernel;
	}

	struct Atlas
	{
		int padding = 1;
		int hmax, wmax;
		int w, h;
		CPImage* atlasPixels;
		int atx, aty;

		void add(std::vector<Glyph*>& glyphs)
		{
			int idx=0;
			for(int i=0;i<glyphs.size();i++)
			{
				auto& lr = glyphs[i];

				//make sure we have horizontal room
				if(atx + lr->w > w)
				{
					atx = padding;
					aty += padding + hmax;
				}

				//(actually transfer glyphs)
				for(int gy=0;gy<lr->h;gy++)
				{
					for(int gx=0;gx<lr->w;gx++)
					{
						atlasPixels->at(atx+gx,aty+gy) = lr->at(gx, gy);
					}
				}

				//record position of glyph in atlas
				lr->atlas_x = atx;
				lr->atlas_y = aty;

				//advance by the wmax, not the letter size (this way, we keep things in a nicer grid)
				//atx += kPadding + wmax;
				//NAH, too complex (for some reason I can't remember)
				//just doesn't look all that nice after all either
				atx += padding + lr->w;
			}
		}


		void Build(std::vector<Glyph*>& glyphs, std::vector<Glyph*>& outlineGlyphs)
		{
			//roughly approximate size of  we'll over-allocate in case we need it
			//pessimistically estimate width by assuming all characters are the widest
			//add some padding at the same time
			//we use the outline glyphs here because they're larger
			wmax = -1;
			hmax = -1;
			for(const auto &OG : outlineGlyphs)
				wmax = std::max(wmax,OG->w), hmax = std::max(hmax,OG->h);
			wmax += padding;
			hmax += padding;
			wmax = wmax;
			hmax = hmax;

			//guessed pixels is based on the largest dimensions * numglyphs and then * 2 for the stroke+outline parts
			const int nGuessedPixels = wmax * hmax * (int)outlineGlyphs.size() * 2;

			//Search each power of 2 width to find something that makes room
			//We start the height off bigger because we know we're going to multiply it by 2 later for a safety margin anyway
			w = 1;
			h = 2;
			for(;;)
			{
				if(w*h>=nGuessedPixels)
					break;
				w *= 2;
				h *= 2;
			}

			//set width to a minimum of 256, so it's easier to see the atlas information "pixels"
			w = std::min(w,256);
			h = h;

			//allocate output buffer to transfer glyphs
			//double-size the height (pessimistically, since we've just been estimating)
			atlasPixels = new CPImage(w,h*2);

			//Begin atlasing with the needed padding
			atx = padding;
			aty = padding;

			//add glyphs
			add(glyphs);
			//outline images go on a new line
			aty += padding + hmax;
			//add outline images
			add(outlineGlyphs);

		}

	};

	Atlas atlas;


	void Process()
	{
		std::string keystr = state.key.GenerateKeyString();

		atlas.Build(glyphs, outlineGlyphs);

		//expand the height to make room for the final characters
		atlas.aty += atlas.padding + atlas.hmax;

		//expand the height to add room for the metadata
		int metaAtY = atlas.aty;
		atlas.aty += 2;

		//produce the metadata
		struct LetterMeta
		{
			uint16_t tx, ty;
			uint16_t w, h;
		};
		for(int i=0;i<256;i++)
		{
			LetterMeta* m = (LetterMeta*)&atlas.atlasPixels->pix[metaAtY*atlas.w];
			//yeah, it's inefficient
			for (auto* G : glyphs)
			{
				if(G->c == i)
				{
					m[i].tx = G->atlas_x;
					m[i].ty = G->atlas_y;
					m[i].w = G->w;
					m[i].h = G->h;

					//(for debugging)
					//xor the alpha channels so we end up with something we can even tell exists
					//m[i].ty ^= 0xFF00;
					//m[i].h ^= 0xFF00;
				}
			}
		}


		//prep a cute image to dump out
		cp_image_t atlasCpImage;
		atlasCpImage.pix = atlas.atlasPixels->pix;
		atlasCpImage.w = atlas.w;
		atlasCpImage.h = (atlas.aty + 7) & ~7;
		cp_save_png(outputInfo.path.c_str(),&atlasCpImage);
		delete atlas.atlasPixels;

		//shove all letters in a json array
		nlohmann::json::array_t jLetters;
		for (const auto* G : glyphs)
		{
			jLetters.push_back(
			{
				{"char", G->c},
				{"x", G->atlas_x},
				{"y", G->atlas_y},
				{"width", G->w},
				{"height", G->h},
				{"xo", G->xo},
				{"yo", G->yo}
			});
		}

		//construct final json doc
		outputInfo.json["letters"] = jLetters;
		outputInfo.json["keyStr"] = keystr;
		outputInfo.json["metaAtY"] = metaAtY;
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
