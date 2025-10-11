#define _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS 1
#define _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING 1

#include "nlohmann/json.hpp"

#include "cute_png.h"

#include "ChatgptFunctions.h"

#include <direct.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <vector>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ltalloc.hpp"


#include "nemtrif/utf8.h"

std::wstring utf8toW(const std::string& str)
{
	std::u16string dest;
	utf8::utf8to16(str.begin(), str.end(), std::back_inserter(dest));
	std::wstring ws(dest.begin(), dest.end());
	return ws;
}

void fatal(const char* fmt, ...)
{
	printf("FATAL ERROR");
	
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);

	abort();
}

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
		ret->yo += this->yo;

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
	int outline = 0;
	bool bold = false;

	std::string GenerateKeyString()
	{
		char tmp[100];
		const char* b = bold?"_bold":"";
		sprintf(tmp,"%s%s_px%d_o%d",toLower(face).c_str(),b,size,outline);
		return tmp;
	}

private:
	static std::string toLower(std::string str)
	{
		std::string ret = str;
		for(char & c : ret)
			c = (char)std::tolower(c);
		return ret;
	}
};

struct InputRef
{
	std::string filename;
	int startCode;
	std::string charset;
	int usenchars = -1;
	int yo = 0;
};

struct CharacterKernData
{
	int before, after, amount;
};

struct State
{
	FizzyFontKeyData key;
	int kern = 0;
	int usenchars = -1;
	int xo = 0;
	int yo = 0;
	int spacesize = 0;
	bool somepx = false;
	int cellWidth = -1, cellHeight = -1;
	std::string charset;
	std::vector<InputRef> inputs;
	std::vector<CharacterKernData> ckern;
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

	~Job()
	{
		for(auto list : {glyphs,outlineGlyphs})
			for(auto g : list)
				delete g;
	}

	void ReadSomepx(int cellWidth, int cellHeight, const InputRef& input)
	{
		cp_image_t* img = imageServer.GetImage(input.filename);

		struct SomepxGlyphScan
		{
			int c;
			int firstPyo = INT_MAX;
			int firstPxo = 0; //invariant
			int lastPyo = -1;
			int lastPxo = -1;
			int catx, caty;
		};

		std::vector<SomepxGlyphScan> scans;

		std::wstring charmapWTmp;
		const wchar_t* charMap = LR"()";

		if(!input.charset.empty())
		{
			charmapWTmp = utf8toW(input.charset);
			charMap = charmapWTmp.c_str();
		}

		const wchar_t* cp = charMap;
		int xslot = 0, yslot = 0;
		while(*cp)
		{
			int catx = cellWidth * xslot;
			int caty = cellHeight * yslot;
			SomepxGlyphScan g;
			g.c = (int)*cp;
			g.catx = catx;
			g.caty = caty;

			//Locate the crop area for the character
			for(int pyo = 0; pyo < cellHeight; pyo++)
			{
				for(int pxo = 0; pxo < cellWidth; pxo++)
				{
					int px = pxo + catx;
					int py = pyo + caty;
					cp_pixel_t c = img->pix[img->w*py+px];
					if(c.a != 0)
					{
						g.lastPyo = pyo;
						g.firstPyo = std::min(g.firstPyo, pyo);
						g.lastPxo = std::max(g.lastPxo, pxo);
					}
				}
			}

			//Add it to the list unless it was empty
			if(g.lastPyo != -1)
				scans.push_back(g);

			cp++;
			xslot++;
			if(xslot == 26) xslot = 0, yslot++;
		} // END SCAN LOOP

		//find the minimum firstPyo; we can crop the top off most glyphs in most cases
		int minFirstPyo = INT_MAX;
		for(const auto &s : scans)
			minFirstPyo = std::min(minFirstPyo, s.firstPyo);

		//create the glyph with the needed dimensions and copy it in
		for(const auto &s : scans)
		{
			int glyphW = s.lastPxo - s.firstPxo + 1;
			int glyphH = s.lastPyo - minFirstPyo + 1;
			Glyph* g = new Glyph(glyphW, glyphH);
			g->c = s.c;
			g->yo = input.yo;
			glyphs.push_back(g);

			for(int gy = 0; gy < glyphH; gy++)
			{
				for(int gx = 0; gx < glyphW; gx++)
				{
					int srcatx = s.catx + gx + s.firstPxo;
					int srcaty = s.caty + gy + minFirstPyo;
					g->at(gx, gy) = img->pix[srcaty * img->w + srcatx];
				}
			}
		}

	}

	void ReadFizzFont(const InputRef& input)
	{
		struct LetterRecord
		{
			int c;
			int x, y, width, height;
			int logw;
			int xo, yo;
		};

		std::vector<LetterRecord> letterRecords;

		cp_image_t* img = imageServer.GetImage(input.filename);

		const int cellPadding = 1;

		int curChar = 0;
		int imageWidth = inputInfo.imageWidth = img->w;
		int imageHeight = inputInfo.imageHeight = img->h;

		uint8_t curAlpha = img->pix[0].a;
		int lastx = 0;
		curChar = input.startCode;
		int index = 0;
		for(int x = 1; x < imageWidth; x++)
		{
			uint8_t a = img->pix[x].a;
			if(a == curAlpha) continue;
			else
			{
				int w = x - lastx;
				auto lr = LetterRecord{ curChar, lastx, 1, w, imageHeight - 1, w, 0, 0 };
				letterRecords.push_back(lr);
				lastx = x;
				curAlpha = img->pix[x].a;
				curChar++;
				index++;
				if(index == input.usenchars && input.usenchars != -1)
					break;
			}
		}
		//huh? I guess it's the last letter
		if(index >= input.usenchars && input.usenchars != -1)
		{
		}
		else
		{
			int w = imageWidth - lastx;
			auto lr = LetterRecord{ curChar, lastx, 1, w, imageHeight - 1, w, 0, 0 };
			letterRecords.push_back(lr);
		}

		//extract all to NewGlyphs
		for(auto& lr : letterRecords)
		{
			Glyph* g = new Glyph(lr.width, lr.height);
			g->c = lr.c;
			glyphs.push_back(g);

			for(int gy = 0; gy < lr.height; gy++)
			{
				for(int gx = 0; gx < lr.width; gx++)
				{
					g->at(gx, gy) = img->pix[(gy + lr.y) * img->w + gx + lr.x];
				}
			}
		}
	}

	void PreProcess()
	{
		//Generate a key string and use it to create the output path
		std::string keystr = state.key.GenerateKeyString();
		outputInfo.path = keystr + ".png";

		//--------------------
		//Reading input format

		for(int II=0;II<state.inputs.size();II++)
		{
			const auto& input = state.inputs[II];

			if(state.somepx)
				ReadSomepx(state.cellWidth, state.cellHeight, input);
			else
				ReadFizzFont(input);
		
		} //END INPUT LOOP

		//for somepx, add a dummy space
		if(state.somepx)
		{
			if(state.spacesize == 0)
				fatal("spacesize must be set for somepx format");
			Glyph *g = new Glyph(state.spacesize, 1);
			g->c = 32;
			glyphs.push_back(g);
		}

		//erase the space and recreate with the needed width (if we have a spacesize set)
		if(state.spacesize)
		{
			auto it = std::find_if(glyphs.begin(), glyphs.end(), [](Glyph* a) {return a->c == 32; });
			if(it != glyphs.end())
			{
				Glyph* g = *it;
				int height = g->h;
				delete g;
				glyphs.erase(it);

				int realSpaceSize = state.spacesize - state.kern;

				g = new Glyph(realSpaceSize, 1);
				g->c = 32;
				glyphs.push_back(g);
			}
		}

		createOutlines();
	}

	void createOutlines()
	{
		//TODO: use actual thickness
		int thickness = 2;

		//create a symmetric circular kernel with anti-aliasing and normalization
		int kernelWidth = thickness * 2 + 1;
		float *kernel = new float[kernelWidth * kernelWidth];
		float center = (float)thickness;
		float maxKernelValue = thickness + 0.5f;
		float overdrive = 1.0f; //increase to darken the outline
		for (int y = 0; y < kernelWidth; y++) {
			for (int x = 0; x < kernelWidth; x++) {
				float dx = x - center;
				float dy = y - center;
				float distance = sqrt(dx * dx + dy * dy);
				float val = std::max(0.0f, maxKernelValue - distance) * overdrive;
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
					auto& px = og->at(x, y);
					px.a = std::min(255, static_cast<int>(accumAlpha));
					px.r = px.g = px.b = 0; //13; //experiment with gray dropshadow
				}
			}
		}

		delete[] kernel;
	}

	struct Atlas
	{
		int padding = 1;
		int hmax, wmax;
		CPImage* atlasPixels;
		int atx, aty;
		int metaAtY;

		void add(std::vector<Glyph*>& glyphs)
		{
			int myw = atlasPixels->w;
			int idx=0;
			for(int i=0;i<glyphs.size();i++)
			{
				auto& lr = glyphs[i];

				//make sure we have horizontal room
				if(atx + lr->w > myw)
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
			{
				int w = 1;
				int h = 2;
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
				//then add 4 more (you'll see why later) and 8*256 more (you'll see why later)
				atlasPixels = new CPImage(w,h*2+8*256);
			}

			//Begin atlasing with the needed padding
			atx = padding;
			aty = padding;

			//add glyphs
			add(glyphs);

			//make room for the current row of images (which have been positioned with their top left at aty)
			aty += padding + hmax;

			//add outline glyphs
			add(outlineGlyphs);

			//same thing again for outline glyphs
			aty += padding + hmax;

			//-----------------------
			//Metadata

			//The metadata is comprised of 512 pixels for the glyphs and 512 pixels for the outlines
			//(each character takes 2 pixels)
			struct LetterMeta
			{
				uint16_t tx, ty;
				uint16_t w, h;
			};

			//Expand the height to add room for the metadata
			//To keep things prettier, we only put 256 pixels per row
			//To make things FASTER (in theory), we could make it 32x32 since textures are generally stored in blocks on GPU
			//But I'm not sure if they are in my Switch engine, so... no sense overthinking it.
			//I've already got it configured to use rows of 256 for metadata for now.
			metaAtY = aty;

			//choose the final atlas size
			//kind of strange to mutate the atlas pixels, but it's safe
			atlasPixels->h = aty;
			atlasPixels->h += 2*2*256; //2 rows, for each of 2 glyph types, times 256 pages!! yeah this is bad, I need to make a compact representation, but whatever for now
			atlasPixels->h = (atlasPixels->h + 7) & ~7; //keep really ugly texture sizes out of the picture

			//Do both glyph types
			for(int type=0;type<2;type++)
			{
				std::vector<Glyph*> &chosenGlyphs = (type==0) ? glyphs : outlineGlyphs;

				for(int i=0;i<256*256;i++)
				{
					LetterMeta* m = (LetterMeta*)&atlasPixels->pix[(metaAtY+type*2*256)*atlasPixels->w];
					//yeah, it's inefficient
					for (auto* G : chosenGlyphs)
					{
						if(G->c == i)
						{
							m[i].tx = G->atlas_x;
							m[i].ty = G->atlas_y;
							m[i].w = G->w;
							m[i].h = G->h;

							if(m[i].ty > 150)
							{
								int zzz=9;
							}

							//(for debugging)
							//xor the alpha channels so we end up with something we can even tell exists
							//THIS BREAKS THE RENDERING! it's only used for checking where the meta is showing up in the output atlas
							//m[i].ty ^= 0xFF00;
							//m[i].h ^= 0xFF00;
						}
					}
				}
			}
		}

	};

	Atlas atlas;


	void Process()
	{
		std::string keystr = state.key.GenerateKeyString();

		//build atlas
		atlas.Build(glyphs, outlineGlyphs);

		//dump cute image
		cp_save_png(outputInfo.path.c_str(), atlas.atlasPixels);

		//Shove all letters in a json array
		//Be sure to do both glyph types
		nlohmann::json::array_t jLetters;
		nlohmann::json::array_t jOutlines;

		for(int type=0;type<2;type++)
		{
			std::vector<Glyph*> &chosenGlyphs = (type==0) ? glyphs : outlineGlyphs;
			nlohmann::json::array_t& ja = (type==0) ? jLetters : jOutlines;

			for (const auto* G : chosenGlyphs)
			{
				ja.push_back(
				{
					{"c", G->c},
					{"x", G->atlas_x},
					{"y", G->atlas_y},
					{"w", G->w},
					{"h", G->h},
					{"xo", G->xo},
					{"yo", G->yo},
				});
			}
		}

		//create character kerning records
		nlohmann::json::array_t jCkerns;
		for(auto rec : this->state.ckern)
		{
			jCkerns.push_back(
				{
					//int before, after, amount;
					{"b",rec.before},
					{"a",rec.after},
					{"n",rec.amount},
				}
			);
		}

		//construct final json doc
		outputInfo.json["glyphs"] = jLetters;
		outputInfo.json["ckerns"] = jCkerns;
		outputInfo.json["outlines"] = jOutlines;
		outputInfo.json["keyStr"] = keystr;
		outputInfo.json["metaAtY"] = atlas.metaAtY;
		outputInfo.json["xo"] = state.xo;
		outputInfo.json["yo"] = 0; //now stored in per-glyph info
		outputInfo.json["kern"] = state.kern;
	}
};

#include <windows.h>
int main(int argc, char* argv[])
{
	SetErrorMode(0);

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
		if(line[0] == '#') //comments
			continue;
		auto parts = splitBySpace(line);
		auto cmd = parts[0];
		if(cmd == "start")
		{
			//reset all state
			new(&state) State();
		}
		else if(cmd == "charset")
		{
			state.charset = trim(line.substr(7));
		}
		else if(cmd == "format")
		{
			if(parts[1] == "somepx")
			{
				state.somepx = true;
				if(parts.size()<4)
					fatal("Not enough args for: format somepx cellwidth cellheight");
				state.cellWidth = std::atoi(parts[2].c_str());
				state.cellHeight = std::atoi(parts[3].c_str());
			}
		}
		else if (cmd == "input")
		{
			if(state.somepx)
			{
				//somepx's format
				InputRef IR;
				IR.filename = parts[1];
				IR.charset = state.charset;
				IR.yo = state.yo;
				state.inputs.push_back(IR);
			}
			else
			{
				InputRef IR = { parts[1],std::atoi(parts[2].c_str()) };
				IR.usenchars = state.usenchars;
				state.inputs.push_back(IR);
			}
		}
		else if(cmd == "outline")
			state.key.outline = atoi(parts[1].c_str());
		else if(cmd == "key_face")
			state.key.face = parts[1];
		else if(cmd == "key_size")
			state.key.size = atoi(parts[1].c_str());
		else if(cmd == "key_bold")
			state.key.bold = true;
		else if(cmd == "yo")
			state.yo = atoi(parts[1].c_str());
		else if(cmd == "xo")
			state.xo = atoi(parts[1].c_str());
		else if(cmd == "kern")
			state.kern = atoi(parts[1].c_str());
		else if(cmd == "ckern")
		{
			CharacterKernData ckd;
			if(parts[1] == "*") ckd.before = -1; else ckd.before = atoi(parts[1].c_str());
			if(parts[2] == "*") ckd.after = -1; else ckd.after = atoi(parts[2].c_str());
			ckd.amount = atoi(parts[3].c_str());
			state.ckern.push_back(ckd);
		}
		else if(cmd == "usenchars")
			state.usenchars = atoi(parts[1].c_str());
		else if(cmd == "spacesize")
			state.spacesize = atoi(parts[1].c_str());
		else if(cmd == "gen")
		{
			//make sure needed parts are set
			if(state.key.size == 0) fatal("key_size not set");
			if(state.key.face.empty()) fatal("key_face not set");

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
