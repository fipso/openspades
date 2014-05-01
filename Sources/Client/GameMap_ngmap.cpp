/*
 Copyright (c) 2013 yvt
 based on code of pysnip (c) Mathias Kaerlev 2011-2012.
 
 This file is part of OpenSpades.
 
 OpenSpades is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 OpenSpades is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with OpenSpades.  If not, see <http://www.gnu.org/licenses/>.
 
 */

#include "GameMap.h"
#include <Core/DeflateStream.h>
#include <Core/IStream.h>
#include <memory>
#include <unordered_map>

namespace spades {
	namespace client {
	
		static const uint32_t signature = 0x1145148a;
		
		enum class ColumnFormat {
			Rle = 0,
			Bitmap = 1
		};;
		
		enum class BlockFormat {
			Constant =   0x00, // stores a color, which are duplicated for every voxels.
			LinearX =    0x01, // stores 1D image whose axis is aligned with X axis
			LinearY =    0x02, // stores 1D image whose axis is aligned with Y axis
			LinearZ =    0x03, // stores 1D image whose axis is aligned with Z axis
			PlanarX =    0x04, // stores 2D image, which are duplicated for every X value.
			PlanarY =    0x05, // stores 2D image, which are duplicated for every Y value.
			PlanarZ =    0x06, // stores 2D image, which are duplicated for every Z value.
			Volumetric = 0x07  // stores 3D image.
		};
		
		enum class ConstantBlockSubFormat {
			Constant = 0x00
		};
		
		enum class LinearBlockSubFormat {
			Linear = 0x00, // 1D image is gradient.
			Raw =    0x20
		};
		
		enum class PlanarBlockSubFormat {
			Linear = 0x00, // 2D image is made by interpolating 4 colors.
			Lossy =  0x10, // 2D image is compressed using a lossy algorithm.
			Raw =    0x20
		};
		
		enum class VolumetricBlockSubFormat {
			Linear = 0x00, // 3D image is made by interpolating 8 colors.
			Raw =    0x10
		};
		
		static uint8_t MakeFormat(BlockFormat f, ConstantBlockSubFormat c) {
			SPAssert(f == BlockFormat::Constant);
			return static_cast<uint8_t>(f) |
			static_cast<uint8_t>(c);
		}
		static uint8_t MakeFormat(BlockFormat f, LinearBlockSubFormat c) {
			SPAssert(f == BlockFormat::LinearX ||
					 f == BlockFormat::LinearY ||
					 f == BlockFormat::LinearZ);
			return static_cast<uint8_t>(f) |
			static_cast<uint8_t>(c);
		}
		static uint8_t MakeFormat(BlockFormat f, PlanarBlockSubFormat c) {
			SPAssert(f == BlockFormat::PlanarX ||
					 f == BlockFormat::PlanarY ||
					 f == BlockFormat::PlanarZ);
			return static_cast<uint8_t>(f) |
			static_cast<uint8_t>(c);
		}
		static uint8_t MakeFormat(BlockFormat f, VolumetricBlockSubFormat c) {
			SPAssert(f == BlockFormat::Volumetric);
			return static_cast<uint8_t>(f) |
			static_cast<uint8_t>(c);
		}
		
		
		// 16byte struct is faster to address
		// TODO: move this to Math.h
		struct IntVector4: public IntVector3 {
			int w;
			
			IntVector4() = default;
			IntVector4(const IntVector4&) = default;
			IntVector4(int x, int y, int z):
			IntVector3(x, y, z) {}
			IntVector4(int x, int y, int z, int w):
			IntVector3(x, y, z), w(w) {}
		};
		
		struct LinearColorBlock {
			IntVector4 colors[8];
		};
		struct PlanarColorBlock {
			IntVector4 colors[8][8];
		};
		
		static uint32_t IntVectorToColor(const IntVector4& v) {
			return v.x | v.y << 8 | v.z << 16;
		}
		static IntVector4 IntVectorFromColor(uint32_t c) {
			return IntVector4(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff);
		}
		
		class BitWriter {
			std::vector<uint8_t>& stream;
			uint8_t buf;
			int pos;
			void Flush() {
				stream.push_back(buf);
				pos = 0;
				buf = 0;
			}
		public:
			BitWriter(std::vector<uint8_t>& stream):stream(stream),
			pos(0), buf(0){}
			~BitWriter() { if(pos > 0) Flush(); }
			void Write(uint32_t d, int size) {
				while(size > 0) {
					int w = std::min(size, 8 - pos);
					buf |= (d & ((1 << w) - 1)) << pos;
					pos += w;
					size -= w;
					d >>= w;
					if(pos == 8) {
						Flush();
					}
				}
			}
			void WriteBit(int b) {
				if(b) buf |= 1 << pos;
				pos++;
				if(pos == 8) Flush();
			}
		};
		class BitReader {
			IStream& stream;
			uint8_t buf;
			int pos;
			void FillBuffer() {
				if(pos == 8) {
					buf = static_cast<uint8_t>(stream.ReadByte());
					pos = 0;
				}
			}
		public:
			BitReader(IStream& stream):stream(stream), pos(8), buf(0) {}
			uint32_t Read(int size) {
				uint32_t ret = 0;
				int shift = 0;
				while(shift < size) {
					if(pos == 8) {
						FillBuffer();
					}
					int w = std::min(size - shift, 8 - pos);
					ret |= static_cast<uint32_t>(buf & ((1 << w) - 1)) << shift;
					buf >>= w; shift += w;
					pos += w;
				}
				return ret;
			}
			uint8_t ReadBit() {
				if(pos == 8) FillBuffer();
				auto ret = buf & 1;
				buf >>= 1; pos++;
				return ret;
			}
		};
		
		namespace ct {
			
//#define USE_HUFFMAN
			
			template<int bits>
			struct Block {
				enum { size = 1 << bits };
				int pixels[size][size];
			};
			
			typedef Block<3> MacroBlock;
			
			namespace {
			}
			
			class CodecBase {
			protected:
				// bit depths for each wavelet level
				int bitdepths[4];
				bool huffmanBitShift;
				int roundZeroLimits[4];
				
				CodecBase(const NGMapOptions& opt) {
					bitdepths[0] = 8;
					bitdepths[1] = 3 + (opt.quality >> 5);
					bitdepths[2] = 2 + (opt.quality >> 5);
					bitdepths[3] = 1 + (opt.quality >> 4);
					huffmanBitShift = opt.quality > 70;
					roundZeroLimits[0] = 0;
					roundZeroLimits[1] = 1;
					roundZeroLimits[2] = 7 - (opt.quality >> 5);
					roundZeroLimits[3] = 12 - (opt.quality >> 4);
				}
			};;
			
			class Decoder: CodecBase {
				BitReader& reader;
				
				/*
				 inline void WriteHuffmanInt(int i) {
				 if(i == 0) {
				 writer.WriteBit(0);
				 }else{
				 int abs = i < 0 ? -i : i;
				 writer.WriteBit(1);
				 for(int k = 1; k < abs; k++) writer.WriteBit(1);
				 writer.WriteBit(0);
				 writer.WriteBit(i < 0 ? 1 : 0);
				 }
				 }*/
				
				inline int ReadHuffmanInt() {
					if(reader.ReadBit()) {
						int i = 1;
						while(reader.ReadBit() == 1) i++;
						if(reader.ReadBit()) i = -i;
						return i;
					}else{
						return 0;
					}
				}
				
				template<int level>
				inline void Pass(Block<level>&);
				
				template<int level>
				inline void Filter(int a[(1 << level) + 8],
								   int b[(1 << level) + 8]);
			public:
				Decoder(BitReader& f, const NGMapOptions& opt):reader(f),CodecBase(opt) {}
				
				
				void Decode(MacroBlock& mb) {
					Pass<3>(mb);
				}
			};
			
			template <int level> inline void Decoder::Pass(Block<level> &b) {
				
				int bitdepth = bitdepths[level];
				
		
				int bits = 0;
				if(huffmanBitShift){
					while(reader.ReadBit() == 0) bits++;
				}else{
#ifdef USE_HUFFMAN
					bits = ReadHuffmanInt();
#else
					bits = reader.Read(3); // effective bits: bitdepth - bitdepth + 7
#endif
				}
				
				int redundantBits = 0;
				while(reader.ReadBit() == 0) redundantBits++;
				
				int writtenBits = bitdepth - redundantBits;
				
				int shiftLeft = 32 - writtenBits;
				int shift = shiftLeft - bits;
				
				// to fill LSBs
				int mask2 = (1 << (bits + bitdepth)) - 1;
				int shift2 = bitdepth;
				
				// read HL
				for(int x = 0; x < Block<level - 1>::size; x++)
					for(int yy = 0; yy < Block<level - 1>::size; yy++) {
						int y = (x&1) ? (Block<level - 1>::size - 1 - yy) : yy;
#ifndef USE_HUFFMAN
						int v = static_cast<int32_t>(reader.Read(writtenBits) << shiftLeft) >> shift;
#else
						int v = ReadHuffmanInt() << bits;
#endif
						//v |= (v & mask2) >> shift2;
						b.pixels[(x << 1) + 1][y << 1] = v;
					}
				// read LH
				for(int x = 0; x < Block<level - 1>::size; x++)
					for(int yy = 0; yy < Block<level - 1>::size; yy++) {
						int y = (x&1) ? (Block<level - 1>::size - 1 - yy) : yy;
#ifndef USE_HUFFMAN
						int v = static_cast<int32_t>(reader.Read(writtenBits) << shiftLeft) >> shift;
#else
						int v = ReadHuffmanInt() << bits;
#endif
						//v |= (v & mask2) >> shift2;
						b.pixels[x << 1][(y << 1) + 1] = v;
					}
				// read HH
				for(int x = 0; x < Block<level - 1>::size; x++)
					for(int yy = 0; yy < Block<level - 1>::size; yy++) {
						int y = (x&1) ? (Block<level - 1>::size - 1 - yy) : yy;
#ifndef USE_HUFFMAN
						int v = static_cast<int32_t>(reader.Read(writtenBits) << shiftLeft) >> shift;
#else
						int v = ReadHuffmanInt() << bits;
#endif
						//v |= (v & mask2) >> shift2;
						b.pixels[(x << 1)][(y << 1)] = v;
					}
				
				// get LL
				Block<level - 1> lower;
				Pass(lower);
				
				for(int x = 0; x < Block<level - 1>::size; x++)
					for(int y = 0; y < Block<level - 1>::size; y++)
						b.pixels[(x << 1) + 1][(y << 1) + 1] = lower.pixels[x][y];
				
				// do filtering
				int tmp[Block<level>::size + 8];
				int tmp2[Block<level>::size + 8];
				for(int x = 0; x < Block<level>::size; x++) {
					for(int y = 0; y < Block<level>::size; y++)
						tmp[y + 4] = b.pixels[x][y];
					Filter<level>(tmp, tmp2);
					for(int y = 0; y < Block<level>::size; y++)
						b.pixels[x][y] = tmp2[y + 4];
				}
				for(int y = 0; y < Block<level>::size; y++) {
					for(int x = 0; x < Block<level>::size; x++)
						tmp[x + 4] = b.pixels[x][y];
					Filter<level>(tmp, tmp2);
					for(int x = 0; x < Block<level>::size; x++)
						b.pixels[x][y] = tmp2[x + 4];
				}
				
				
			}
			
			template <> void Decoder::Pass<0>(Block<0> &b) {
				b.pixels[0][0] = static_cast<int>(reader.Read(9) << 23) >> 22;
			}
			
			
			template<int level> inline void Decoder::Filter(int arr[(1 << level) + 8],
															int ret[(1 << level) + 8]) {
				static const int size = 1 << level;
				
				if(level == 3) {
					arr[3] = arr[5]; arr[2] = arr[6];
					arr[1] = arr[7]; arr[0] = arr[8];
					arr[12] = arr[10]; arr[13] = arr[9];
					arr[14] = arr[8]; arr[15] = arr[7];
				} else if(level == 2) {
					arr[3] = arr[5]; arr[2] = arr[6];
					arr[1] = arr[7]; arr[0] = arr[6];
					arr[8] = arr[6]; arr[9] = arr[5];
					arr[10] = arr[4]; arr[11] = arr[5];
				} else if(level == 1) {
					arr[3] = arr[5]; arr[2] = arr[6];
					arr[1] = arr[5]; arr[0] = arr[6];
					arr[6] = arr[4]; arr[7] = arr[5];
					arr[8] = arr[4]; arr[9] = arr[5];
				}
				for(int i = 3; i < size + 4; i+=2) {
					ret[i] = arr[i] - ((arr[i - 1] + arr[i + 1] + 2) >> 2);
				}
				for(int i = 4; i < size + 4; i+=2) {
					ret[i] = arr[i] + ((ret[i - 1] + ret[i + 1]) >> 1);
				}
			}
			
			/** for algorithm design */
			class Histogram {
				std::unordered_map<int, int> hist;
				const char *n;
			public:
				Histogram(const char *name):n(name){}
				~Histogram() {
					fprintf(stderr, "==== HISTOGRAM: %s ====\n", n);
					std::vector<std::pair<int, int>> lst;
					for(const auto& e: hist) {
						lst.emplace_back(e);
					}
					std::sort(lst.begin(), lst.end());
					for(const auto& e: lst) {
						fprintf(stderr, "%d,%d\n", e.first, e.second);
					}
					fprintf(stderr, "==== END: %s ====\n", n);
					
				}
				
				void operator ()(int v) {
					hist[v] ++;
				}
			};
			
			static Histogram histogram_coded("Mantissa");
			
			class Encoder: CodecBase {
				BitWriter& writer;
				
				inline void WriteHuffmanInt(int i) {
					if(i == 0) {
						writer.WriteBit(0);
					}else{
						int abs = i < 0 ? -i : i;
						writer.WriteBit(1);
						for(int k = 1; k < abs; k++) writer.WriteBit(1);
						writer.WriteBit(0);
						writer.WriteBit(i < 0 ? 1 : 0);
					}
				}
				
				template<int level>
				inline void Pass(Block<level>& block);
				
				template<int level>
				inline void Filter(int arr[(1 << level) + 8],
								   int ret[(1 << level) + 8]);
			public:
				Encoder(BitWriter& f, const NGMapOptions& opt):writer(f),CodecBase(opt) {}
				
				void Encode(MacroBlock& mb) {
					Pass(mb);
				}
			};
			
			template<int level> inline void Encoder::Pass(Block<level>& b) {
				
				// do filtering
				int tmp[Block<level>::size + 8];
				int tmp2[Block<level>::size + 8];
				for(int y = 0; y < Block<level>::size; y++) {
					for(int x = 0; x < Block<level>::size; x++)
						tmp[x + 4] = b.pixels[x][y];
					Filter<level>(tmp, tmp2);
					for(int x = 0; x < Block<level>::size; x++)
						b.pixels[x][y] = tmp2[x + 4];
				}
				for(int x = 0; x < Block<level>::size; x++) {
					for(int y = 0; y < Block<level>::size; y++)
						tmp[y + 4] = b.pixels[x][y];
					Filter<level>(tmp, tmp2);
					for(int y = 0; y < Block<level>::size; y++)
						b.pixels[x][y] = tmp2[y + 4];
				}
				
				// calc effective bits
				int bitdepth = bitdepths[level];
				int bits = 1; //bitdepth - 1;
				for(int x = 0; x < Block<level - 1>::size; x++)
					for(int y = 0; y < Block<level - 1>::size; y++) {
						int k = b.pixels[(x << 1) + 1][y << 1];
						if(k < 0) k = ~k; while((unsigned int)k >> bits) bits++;
						k = b.pixels[(x << 1)][(y << 1)];
						if(k < 0) k = ~k; while((unsigned int)k >> bits) bits++;
						k = b.pixels[x << 1][(y << 1) +  1];
						if(k < 0) k = ~k; while((unsigned int)k >> bits) bits++;
					}
				bits++;
				
				int redundantBits = std::max(bitdepth - bits, 0);
				int writtenBits = std::min(bits, bitdepth);
				
				if(huffmanBitShift) {
					if(bits > bitdepth + 7) bits = bitdepth + 7;
					for(int i = bits - bitdepth; i > 0; i--) writer.WriteBit(0);
					writer.WriteBit(1);
				}else{
#ifdef USE_HUFFMAN
					WriteHuffmanInt(std::max(bits - bitdepth, 0));
#else
					writer.Write(std::max(bits - bitdepth, 0), 3);
#endif
				}
				
				//histogram_coded(bits);
				for(int i = redundantBits; i > 0; i--) writer.WriteBit(0);
				writer.WriteBit(1);
				
				int shift = std::max(bits - bitdepth, 0);
				int round = (1 << shift) >> 1;
				int8_t maxValue = ((1 << writtenBits) >> 1) - 1;
				int8_t minValue = ~maxValue;
				
				int8_t roundZeroMin = -roundZeroLimits[level];
				int8_t roundZeroMax = roundZeroLimits[level];
				
				// write HL
				for(int x = 0; x < Block<level - 1>::size; x++)
					for(int yy = 0; yy < Block<level - 1>::size; yy++) {
						int y = (x&1) ? (Block<level - 1>::size - 1 - yy) : yy;
						int k = b.pixels[(x << 1) + 1][y << 1];
						if(k >= roundZeroMin && k <= roundZeroMax) k = 0;
						k = (k + round) >> shift;
						k = std::max<int>(std::min<int>(k, maxValue), minValue);
#ifdef USE_HUFFMAN
						WriteHuffmanInt(k);
#else
						writer.Write(k, writtenBits);
#endif
					}
				// write LH
				for(int x = 0; x < Block<level - 1>::size; x++)
					for(int yy = 0; yy < Block<level - 1>::size; yy++) {
						int y = (x&1) ? (Block<level - 1>::size - 1 - yy) : yy;
						int k = b.pixels[x << 1][(y << 1) + 1];
						if(k >= roundZeroMin && k <= roundZeroMax) k = 0;
						k = (k + round) >> shift;
						k = std::max<int>(std::min<int>(k, maxValue), minValue);
#ifdef USE_HUFFMAN
						WriteHuffmanInt(k);
#else
						writer.Write(k, writtenBits);
#endif
					}
				// write HH
				for(int x = 0; x < Block<level - 1>::size; x++)
					for(int yy = 0; yy < Block<level - 1>::size; yy++) {
						int y = (x&1) ? (Block<level - 1>::size - 1 - yy) : yy;
						int k = b.pixels[(x << 1)][(y << 1)];
						if(k >= roundZeroMin && k <= roundZeroMax) k = 0;
						k = (k + round) >> shift;
						k = std::max<int>(std::min<int>(k, maxValue), minValue);
#ifdef USE_HUFFMAN
						WriteHuffmanInt(k);
#else
						writer.Write(k, writtenBits);
#endif
					}
				
				// write LL
				Block<level - 1> lower;
				for(int x = 0; x < Block<level - 1>::size; x++)
					for(int y = 0; y < Block<level - 1>::size; y++)
						lower.pixels[x][y] = b.pixels[(x << 1) + 1][(y << 1) + 1];
				/*
				for(int y = 0; y < Block<level>::size; y++) {
					for(int x = 0; x < Block<level>::size; x++) {
						printf("%5d ", b.pixels[((x << 1) & 7) | (x >> 2)][((y << 1) & 7) | (y >> 2)]);
					}
					puts("");
				}//*/
				
				Pass(lower);
			}
			
			template<> inline void Encoder::Pass<0>(Block<0>& b) {
				writer.Write((b.pixels[0][0] + 1) >> 1, 9);
			}
			
			template<int level> inline void Encoder::Filter(int arr[(1 << level) + 8],
															int ret[(1 << level) + 8]) {
				static const int size = 1 << level;
				
				if(level == 3) {
					arr[3] = arr[5]; arr[2] = arr[6];
					arr[1] = arr[7]; arr[0] = arr[8];
					arr[12] = arr[10]; arr[13] = arr[9];
					arr[14] = arr[8]; arr[15] = arr[7];
				} else if(level == 2) {
					arr[3] = arr[5]; arr[2] = arr[6];
					arr[1] = arr[7]; arr[0] = arr[6];
					arr[8] = arr[6]; arr[9] = arr[5];
					arr[10] = arr[4]; arr[11] = arr[5];
				} else if(level == 1) {
					arr[3] = arr[5]; arr[2] = arr[6];
					arr[1] = arr[5]; arr[0] = arr[6];
					arr[6] = arr[4]; arr[7] = arr[5];
					arr[8] = arr[4]; arr[9] = arr[5];
				}
				
				for(int i = 4; i <= size + 4; i+=2) {
					ret[i] = arr[i] - ((arr[i - 1] + arr[i + 1]) >> 1);
				}
				for(int i = 5; i < size + 4; i+=2) {
					ret[i] = arr[i] + ((ret[i - 1] + ret[i + 1] + 2) >> 2);
				}
			}
		}
		
		struct ColorBlock {
			uint8_t needscolor[8][8];
			uint32_t colors[8][8][8];
			
			IntVector4 ColorAsIntVector(int x, int y, int z) {
				auto c = colors[x][y][z];
				return IntVectorFromColor(c);
			}
			
			uint8_t GetNeedsColorMapLinearX() {
				uint8_t ret = 0;
				for(int x = 0; x < 8; x++) {
					if(needscolor[x][0] || needscolor[x][1] ||
					   needscolor[x][2] || needscolor[x][3] ||
					   needscolor[x][4] || needscolor[x][5] ||
					   needscolor[x][6] || needscolor[x][7])
						ret |= 1 << x;
				}
				return ret;
			}
			
			uint8_t GetNeedsColorMapLinearY() {
				uint8_t ret = 0;
				for(int x = 0; x < 8; x++) {
					if(needscolor[0][x] || needscolor[1][x] ||
					   needscolor[2][x] || needscolor[3][x] ||
					   needscolor[4][x] || needscolor[5][x] ||
					   needscolor[6][x] || needscolor[7][x])
						ret |= 1 << x;
				}
				return ret;
			}
			
			uint8_t GetNeedsColorMapLinearZ() {
				uint8_t ret = 0;
				for(int x = 0; x < 8; x++)
					for(int y =0; y < 8; y++)
						ret |= needscolor[x][y];
				return ret;
			}
			
			void GetNeedsColorMapPlanarX(uint8_t ret[8]) {
				std::fill(ret, ret + 8, 0);
				for(int x = 0; x < 8; x++) {
					for(int y = 0; y < 8; y++) {
						ret[y] |= needscolor[x][y];
					}
				}
			}
			void GetNeedsColorMapPlanarY(uint8_t ret[8]) {
				std::fill(ret, ret + 8, 0);
				for(int y = 0; y < 8; y++) {
					for(int x = 0; x < 8; x++) {
						ret[x] |= needscolor[x][y];
					}
				}
			}
			void GetNeedsColorMapPlanarZ(uint8_t ret[8]) {
				for(int x = 0; x < 8; x++) {
					uint8_t r = 0;
					for(int y = 0; y < 8; y++) {
						if(needscolor[x][y]) r |= 1 << y;
					}
					ret[x] = r;
				}
			}
			
			
			IntVector4 ToConstant() {
				IntVector4 sum(0, 0, 0);
				int count = 0;
				for(int x = 0; x < 8; x++) {
					for(int y = 0; y < 8; y++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								sum += c; count++;
							}
						}
					}
				}
				if(count > 1) sum /= count;
				return sum;
			}
			
			void FromConstant(const IntVector4& c) {
				auto cc = IntVectorToColor(c);
				for(int x = 0; x < 8; x++)
					for(int y = 0; y < 8; y++)
						for(int z = 0; z < 8; z++)
							colors[x][y][z] = cc;
			}
			
			void ToLinearX(LinearColorBlock& o) {
				for(int x = 0; x < 8; x++) {
					IntVector4 sum(0, 0, 0);
					int count = 0;
					for(int y = 0; y < 8; y++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								sum += c; count++;
							}
						}
					}
					if(count > 1) sum /= count;
					o.colors[x] = sum;
				}
			}
			
			void FromLinearX(const LinearColorBlock& o) {
				for(int x = 0; x < 8; x++) {
					auto c = IntVectorToColor(o.colors[x]);
					for(int y = 0; y < 8; y++)
						for(int z = 0; z < 8; z++)
							colors[x][y][z] = c;
				}
			}
			
			void ToLinearY(LinearColorBlock& o) {
				for(int y = 0; y < 8; y++) {
					IntVector4 sum(0, 0, 0);
					int count = 0;
					for(int x = 0; x < 8; x++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								sum += c; count++;
							}
						}
					}
					if(count > 1) sum /= count;
					o.colors[y] = sum;
				}
			}
			
			void FromLinearY(const LinearColorBlock& o) {
				for(int x = 0; x < 8; x++) {
					auto c = IntVectorToColor(o.colors[x]);
					for(int y = 0; y < 8; y++)
						for(int z = 0; z < 8; z++)
							colors[y][x][z] = c;
				}
			}
			
			void ToLinearZ(LinearColorBlock& o) {
				IntVector4 sum[8];
				int count[8] = {0, 0, 0, 0, 0, 0, 0, 0};
				for(int i = 0; i < 8; i++) sum[i] = IntVector4(0, 0, 0);
				for(int x = 0; x < 8; x++) {
					for(int y = 0; y < 8; y++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								sum[z] += c; count[z]++;
							}
						}
					}
				}
				for(int i = 0; i < 8; i++) {
					if(count[i] > 1) sum[i] /= count[i];
					o.colors[i] = sum[i];
				}
			}
			
			void FromLinearZ(const LinearColorBlock& o) {
				for(int x = 0; x < 8; x++) {
					auto c = IntVectorToColor(o.colors[x]);
					for(int y = 0; y < 8; y++)
						for(int z = 0; z < 8; z++)
							colors[y][z][x] = c;
				}
			}
			
			void ToPlanarX(PlanarColorBlock& o) {
				int count[8][8];
				memset(count, 0, sizeof(count));
				memset(o.colors, 0, sizeof(o.colors));
				for(int y = 0; y < 8; y++) {
					for(int x = 0; x < 8; x++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								o.colors[y][z] += c;
								count[y][z] ++;
							}
						}
					}
				}
				for(int x = 0; x < 8; x++)
					for(int y = 0; y < 8; y++)
						if(count[x][y] > 1)
							o.colors[x][y] /= count[x][y];
			}
			
			void FromPlanarX(const PlanarColorBlock& o) {
				for(int x = 0; x < 8; x++) {
					for(int y = 0; y < 8; y++) {
						auto c = IntVectorToColor(o.colors[x][y]);
						for(int i = 0; i < 8; i++)
							colors[i][x][y] = c;
					}
				}
			}
			void ToPlanarY(PlanarColorBlock& o) {
				int count[8][8];
				memset(count, 0, sizeof(count));
				memset(o.colors, 0, sizeof(o.colors));
				for(int y = 0; y < 8; y++) {
					for(int x = 0; x < 8; x++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								o.colors[x][z] += c;
								count[x][z] ++;
							}
						}
					}
				}
				for(int x = 0; x < 8; x++)
					for(int y = 0; y < 8; y++)
						if(count[x][y] > 1)
							o.colors[x][y] /= count[x][y];
			}
			void FromPlanarY(const PlanarColorBlock& o) {
				for(int x = 0; x < 8; x++) {
					for(int y = 0; y < 8; y++) {
						auto c = IntVectorToColor(o.colors[x][y]);
						for(int i = 0; i < 8; i++)
							colors[x][i][y] = c;
					}
				}
			}
			void ToPlanarZ(PlanarColorBlock& o) {
				for(int y = 0; y < 8; y++) {
					for(int x = 0; x < 8; x++) {
						auto b = needscolor[x][y];
						int count = 0;
						IntVector4 sum(0, 0, 0);
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								sum += c;
								count ++;
							}
						}
						if(count > 1) sum /= count;
						o.colors[x][y] = sum;
					}
				}
			}
			void FromPlanarZ(const PlanarColorBlock& o) {
				for(int x = 0; x < 8; x++) {
					for(int y = 0; y < 8; y++) {
						auto c = IntVectorToColor(o.colors[x][y]);
						for(int i = 0; i < 8; i++)
							colors[x][y][i] = c;
					}
				}
			}
			
			int ComputeDiversity() {
				IntVector3 max, min;
				bool has = false;
				for(int x = 0; x < 8; x++) {
					for(int y = 0; y < 8; y++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								if(has) {
									max.x = std::max(max.x, c.x);
									max.y = std::max(max.y, c.y);
									max.z = std::max(max.z, c.z);
									min.x = std::min(min.x, c.x);
									min.y = std::min(min.y, c.y);
									min.z = std::min(min.z, c.z);
								}else{
									max = min = c;
									has = true;
								}
							}
						}
					}
				}
				if(has) {
					auto diff = max - min;
					return IntVector3::Dot(diff, diff);
				} else {
					return 0;
				}
			}
			
			int ComputeDiversity1D_X() {
				int maxdiv = 0;
				for(int x = 0; x < 8; x++) {
					IntVector3 max, min;
					bool has = false;
					for(int y = 0; y < 8; y++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								if(has) {
									max.x = std::max(max.x, c.x);
									max.y = std::max(max.y, c.y);
									max.z = std::max(max.z, c.z);
									min.x = std::min(min.x, c.x);
									min.y = std::min(min.y, c.y);
									min.z = std::min(min.z, c.z);
								}else{
									max = min = c;
									has = true;
								}
							}
						}
					}
					if(has) {
						auto diff = max - min;
						maxdiv = std::max(maxdiv, IntVector3::Dot(diff, diff));
					}
				}
				
				return maxdiv;
			}
			int ComputeDiversity1D_Y() {
				int maxdiv = 0;
				for(int y = 0; y < 8; y++) {
					IntVector3 max, min;
					bool has = false;
					for(int x = 0; x < 8; x++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								if(has) {
									max.x = std::max(max.x, c.x);
									max.y = std::max(max.y, c.y);
									max.z = std::max(max.z, c.z);
									min.x = std::min(min.x, c.x);
									min.y = std::min(min.y, c.y);
									min.z = std::min(min.z, c.z);
								}else{
									max = min = c;
									has = true;
								}
							}
						}
					}
					if(has) {
						auto diff = max - min;
						maxdiv = std::max(maxdiv, IntVector3::Dot(diff, diff));
					}
				}
				
				return maxdiv;
			}
			
			int ComputeDiversity1D_Z() {
				IntVector3 max[8], min[8];
				uint8_t has = 0;
				for(int x = 0; x < 8; x++) {
					for(int y = 0; y < 8; y++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								if(has&(1<<z)) {
									max[z].x = std::max(max[z].x, c.x);
									max[z].y = std::max(max[z].y, c.y);
									max[z].z = std::max(max[z].z, c.z);
									min[z].x = std::min(min[z].x, c.x);
									min[z].y = std::min(min[z].y, c.y);
									min[z].z = std::min(min[z].z, c.z);
								}else{
									max[z] = min[z] = c;
									has |= 1<<z;
								}
							}
						}
					}
				}
				
				int maxdiv = 0;
				for(int i = 0; i < 8; i++) {
					if(has & (1<<i)) {
						auto diff = max[i] - min[i];
						maxdiv = std::max(maxdiv, IntVector3::Dot(diff, diff));
					}
				}
				return maxdiv;
			}
			
			int ComputeDiversity2D_X() {
				int maxdiv = 0;
				for(int y = 0; y < 8; y++) {
					IntVector3 max[8], min[8];
					uint8_t has = 0;
					for(int x = 0; x < 8; x++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								if(has&(1<<z)) {
									max[z].x = std::max(max[z].x, c.x);
									max[z].y = std::max(max[z].y, c.y);
									max[z].z = std::max(max[z].z, c.z);
									min[z].x = std::min(min[z].x, c.x);
									min[z].y = std::min(min[z].y, c.y);
									min[z].z = std::min(min[z].z, c.z);
								}else{
									max[z] = min[z] = c;
									has |= 1<<z;
								}
							}
						}
					}
					for(int i = 0; i < 8; i++) {
						if(has & (1<<i)) {
							auto diff = max[i] - min[i];
							maxdiv = std::max(maxdiv, IntVector3::Dot(diff, diff));
						}
					}
				}
				
				return maxdiv;
			}
			
			int ComputeDiversity2D_Y() {
				int maxdiv = 0;
				for(int x = 0; x < 8; x++) {
					IntVector3 max[8], min[8];
					uint8_t has = 0;
					for(int y = 0; y < 8; y++) {
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								if(has&(1<<z)) {
									max[z].x = std::max(max[z].x, c.x);
									max[z].y = std::max(max[z].y, c.y);
									max[z].z = std::max(max[z].z, c.z);
									min[z].x = std::min(min[z].x, c.x);
									min[z].y = std::min(min[z].y, c.y);
									min[z].z = std::min(min[z].z, c.z);
								}else{
									max[z] = min[z] = c;
									has |= 1<<z;
								}
							}
						}
					}
					for(int i = 0; i < 8; i++) {
						if(has & (1<<i)) {
							auto diff = max[i] - min[i];
							maxdiv = std::max(maxdiv, IntVector3::Dot(diff, diff));
						}
					}
				}
				
				return maxdiv;
			}
			
			int ComputeDiversity2D_Z() {
				int maxdiv = 0;
				for(int x = 0; x < 8; x++) {
					for(int y = 0; y < 8; y++) {
						IntVector3 max, min;
						bool has = false;
						auto b = needscolor[x][y];
						if(b) {
							for(int z = 0; z < 8; z++) {
								if((b & (1 << z)) == 0) continue;
								auto c = ColorAsIntVector(x, y, z);
								if(has) {
									max.x = std::max(max.x, c.x);
									max.y = std::max(max.y, c.y);
									max.z = std::max(max.z, c.z);
									min.x = std::min(min.x, c.x);
									min.y = std::min(min.y, c.y);
									min.z = std::min(min.z, c.z);
								}else{
									max = min = c;
									has = true;
								}
							}
							auto diff = max - min;
							maxdiv = std::max(maxdiv, IntVector3::Dot(diff, diff));
						}
					}
				}
				
				return maxdiv;
			}
			
		};
		
		
		void NGMapOptions::Validate() const {
			if(quality < 0 || quality > 100) {
				SPRaise("Invalid map quality value: %d", quality);
			}
		}
		
#pragma mark - Tools
		
		
		
#pragma mark - Decoder
		
		class GameMap::NGMapDecoder {
			friend class ColorBlockEmitter;
			IStream& stream;
			NGMapOptions options;
			
			void DecodeConstant(uint8_t, ColorBlock& block) {
				IntVector4 c;
				c.x = stream.ReadByte();
				c.y = stream.ReadByte();
				c.z = stream.ReadByte();
				block.FromConstant(c);
			}
			
			static void DecodeLinearLinear(const IntVector3& c1,
										   const IntVector3& c2,
										   LinearColorBlock& sub) {
				int v1 = c1.x * 7, d1 = c2.x - c1.x;
				int v2 = c1.y * 7, d2 = c2.y - c1.y;
				int v3 = c1.z * 7, d3 = c2.z - c1.z;
				for(int i = 0; i < 8; i++) {
					IntVector4& c = sub.colors[i];
					// good compile can do this without division
					c.x = v1 / 7;
					c.y = v2 / 7;
					c.z = v3 / 7;
					v1 += d1; v2 += d2; v3 += d3;
				}
				
			}
			
			void DecodeLinear(uint8_t fmtcode, ColorBlock& block) {
				auto fmt = static_cast<BlockFormat>(fmtcode & 0xf);
				auto subfmt = static_cast<LinearBlockSubFormat>(fmtcode & 0xf0);
				LinearColorBlock sub;
				uint8_t needscolor = 0;
				switch(fmt) {
					case BlockFormat::LinearX:
						needscolor = block.GetNeedsColorMapLinearX();
						break;
					case BlockFormat::LinearY:
						needscolor = block.GetNeedsColorMapLinearY();
						break;
					case BlockFormat::LinearZ:
						needscolor = block.GetNeedsColorMapLinearZ();
						break;
					default:
						// shouldn't happen...
						break;
				}
				
				switch(subfmt) {
					case LinearBlockSubFormat::Linear:
					{
						IntVector4 c1;
						c1.x = stream.ReadByte();
						c1.y = stream.ReadByte();
						c1.z = stream.ReadByte();
						IntVector4 c2;
						c2.x = stream.ReadByte();
						c2.y = stream.ReadByte();
						c2.z = stream.ReadByte();
						DecodeLinearLinear(c1, c2, sub);
						break;
					}
					case LinearBlockSubFormat::Raw:
						for(int i = 0; i < 8; i++) {
							if(!(needscolor & (1 << i))) continue;
							IntVector4& c = sub.colors[i];
							c.x = stream.ReadByte();
							c.y = stream.ReadByte();
							c.z = stream.ReadByte();
						}
						break;
					default:
						SPRaise("Invalid format: 0x%02x", static_cast<int>(fmtcode));
				}
				
				switch(fmt) {
					case BlockFormat::LinearX:
						block.FromLinearX(sub);
						break;
					case BlockFormat::LinearY:
						block.FromLinearY(sub);
						break;
					case BlockFormat::LinearZ:
						block.FromLinearZ(sub);
						break;
					default:
						// shouldn't happen...
						break;
				}
			}
			
			static void DecodePlanarLinear(const IntVector3& c1,
										   const IntVector3& c2,
										   const IntVector3& c3,
										   const IntVector3& c4,
										   PlanarColorBlock& sub) {
				int a1 = c1.x * 7, ad1 = c2.x - c1.x;
				int a2 = c1.y * 7, ad2 = c2.y - c1.y;
				int a3 = c1.z * 7, ad3 = c2.z - c1.z;
				int b1 = c3.x * 7, bd1 = c4.x - c3.x;
				int b2 = c3.y * 7, bd2 = c4.y - c3.y;
				int b3 = c3.z * 7, bd3 = c4.z - c3.z;
				for(int x = 0; x < 8; x++) {
					int c1 = a1 * 7, c2 = a2 * 7, c3 = a3 * 7;
					int cd1 = b1 - a1, cd2 = b2 - a2, cd3 = b3 - a3;
					for(int y = 0; y < 8; y++) {
						IntVector4& c = sub.colors[x][y];
						c.x = c1 / 49;
						c.y = c2 / 49;
						c.z = c3 / 49;
						c1 += cd1; c2 += cd2; c3 += cd3;
					}
					a1 += ad1; a2 += ad2; a3 += ad3;
					b1 += bd1; b2 += bd2; b3 += bd3;
				}
			}
			
			void DecodePlanarCT(PlanarColorBlock& sub) {
				BitReader reader(stream);
				
				ct::MacroBlock blockY, blockU, blockV;
				
				auto doBlock = [&](ct::MacroBlock& block) {
					ct::Decoder decoder(reader, options);
					decoder.Decode(block);
				};
				
				doBlock(blockY);
				doBlock(blockU);
				doBlock(blockV);
				
				for(int x = 0; x < 8; x++) {
					for(int y = 0; y < 8; y++) {
						auto t = -blockU.pixels[x][y];
						auto& c = sub.colors[x][y];
						c.y = blockY.pixels[x][y] - (t >> 1);
						c.x = t + c.y - ((blockV.pixels[x][y] + 1) >> 1);
						c.z = blockV.pixels[x][y] + c.x;
						c.x = std::max(std::min(c.x, 255), 0);
						c.y = std::max(std::min(c.y, 255), 0);
						c.z = std::max(std::min(c.z, 255), 0);
						std::swap(c.z, c.x);
					}
				}
			}
			
			void DecodePlanar(uint8_t fmtcode, ColorBlock& block) {
				auto fmt = static_cast<BlockFormat>(fmtcode & 0xf);
				auto subfmt = static_cast<PlanarBlockSubFormat>(fmtcode & 0xf0);
				PlanarColorBlock sub;
				uint8_t needscolor[8];
				
				switch(fmt) {
					case BlockFormat::PlanarX:
						block.GetNeedsColorMapPlanarX(needscolor);
						break;
					case BlockFormat::PlanarY:
						block.GetNeedsColorMapPlanarY(needscolor);
						break;
					case BlockFormat::PlanarZ:
						block.GetNeedsColorMapPlanarZ(needscolor);
						break;
					default:
						// shouldn't happen...
						std::fill(needscolor, needscolor + 8, 0);
						break;
				}
				
				switch(subfmt) {
					case PlanarBlockSubFormat::Linear:
					{
						IntVector4 c1;
						c1.x = stream.ReadByte();
						c1.y = stream.ReadByte();
						c1.z = stream.ReadByte();
						IntVector4 c2;
						c2.x = stream.ReadByte();
						c2.y = stream.ReadByte();
						c2.z = stream.ReadByte();
						IntVector4 c3;
						c3.x = stream.ReadByte();
						c3.y = stream.ReadByte();
						c3.z = stream.ReadByte();
						IntVector4 c4;
						c4.x = stream.ReadByte();
						c4.y = stream.ReadByte();
						c4.z = stream.ReadByte();
						DecodePlanarLinear(c1, c2, c3, c4, sub);
						break;
					}
					case PlanarBlockSubFormat::Lossy:
						DecodePlanarCT(sub);
						break;
					case PlanarBlockSubFormat::Raw:
						for(int x = 0; x < 8; x++) {
							for(int yy = 0; yy < 8; yy++) {
								int y = (x & 1) ? (7 - yy) : yy;
								if(!(needscolor[x]&(1<<y))) continue;
								IntVector4& c = sub.colors[x][y];
								c.x = stream.ReadByte();
								c.y = stream.ReadByte();
								c.z = stream.ReadByte();
							}
						}
						break;
					default:
						SPRaise("Invalid format: 0x%02x", static_cast<int>(fmtcode));
				}
				
				switch(fmt) {
					case BlockFormat::PlanarX:
						block.FromPlanarX(sub);
						break;
					case BlockFormat::PlanarY:
						block.FromPlanarY(sub);
						break;
					case BlockFormat::PlanarZ:
						block.FromPlanarZ(sub);
						break;
					default:
						// shouldn't happen...
						break;
				}
			}
			
			void DecodeVolumetric(uint8_t fmtcode, ColorBlock& block) {
				auto subfmt = static_cast<VolumetricBlockSubFormat>(fmtcode & 0xf0);
				
				switch(subfmt) {
					case VolumetricBlockSubFormat::Raw:
						for(int x = 0; x < 8; x++) {
							for(int yy = 0; yy < 8; yy++) {
								int y = (x & 1) ? (7 - yy) : yy;
								auto b = block.needscolor[x][y];
								if(!b) continue;
								for(int zz = 0; zz < 8; zz++) {
									int z = (yy & 1) ? (7 - zz) : zz;
									if(!(b&(1<<z))) continue;
									IntVector4 c;
									c.x = stream.ReadByte();
									c.y = stream.ReadByte();
									c.z = stream.ReadByte();
									block.colors[x][y][z] = IntVectorToColor(c);
								}
							}
						}
						break;
					default:
						SPRaise("Invalid format: 0x%02x", static_cast<int>(fmtcode));
				}
				
			}
				
		public:
			NGMapDecoder(IStream& stream): stream(stream) {
				
			}
			
			
			GameMap *Decode(std::function<void(float)> progressListener) {
				progressListener(0.f);
				
				auto sig = stream.ReadLittleInt();
				if(sig != signature) {
					SPRaise("Invalid signature: 0x%08x (expected 0x%08x)", sig, signature);
				}
				
				int w = stream.ReadLittleShort();
				int h = stream.ReadLittleShort();
				int d = stream.ReadLittleShort();
				if(w < 1 || h < 1 || d < 1 ||
				   w > 8192 || h > 8192 || d > 128) {
					SPRaise("Invalid map dimension: %dx%dx%d", w, h, d);
				}
				
				if(d > 64) {
					// due to bit width of solidMap
					SPRaise("Depth over 64 is not supported: %d", d);
				}
				
				options.quality = stream.ReadByte();
				options.Validate();
				
				Handle<GameMap> mp(new GameMap(w, h, d), false);
				
				// read geometry
				for(int y = 0; y < h; y++) {
					bool odd = y & 1;
					for(int i = 0, x = odd ? w - 1 : 0, s = odd ? -1 : 1; i < w; i++, x += s) {
						int fmt = stream.ReadByte();
						ColumnFormat fmtType = static_cast<ColumnFormat>(fmt & 1);
						uint64_t solidMap = 0;
						
						if(fmtType == ColumnFormat::Rle) {
							// RLE format
							int z = 0;
							bool fill = false;
							while(z < d) {
								auto inb = stream.ReadByte();
								if(inb & 0x80) {
									fill = true;
									inb &= 0x7f;
								}
								
								if(inb <= z || inb > d) {
									SPRaise("Data corrupted: invalid Z coordinate: %d", inb);
								}
								
								if(fill) {
									while(z < inb) {
										solidMap |= 1ULL << z;
										z++;
									}
								} else {
									z = inb;
								}
								
								fill = !fill;
							}
						} else if(fmtType == ColumnFormat::Bitmap) {
							for(int z = 0; z < d; z += 16) {
								if((fmt >> (1 + (z >> 4))) & 1) {
									auto b = stream.ReadLittleShort();
									solidMap |= static_cast<uint64_t>(b) << z;
								}
							}
						}
						
						mp->solidMap[x][y] = solidMap;
					}
					
					progressListener(0.5f * static_cast<float>(y) / static_cast<float>(h));
				}
				
				
				// read colors
				for(int y = 0; y < h; y += 8) {
					for(int x = 0; x < w; x += 8) {
						for(int zz = 0; zz < d; zz += 8) {
							int z = (x & 8) ? (((d+7)&~7) - 8 - zz) : zz;
							ColorBlock block;
							mp->ComputeNeedsColor(x, y, z, block.needscolor);
							
							auto fmtcode = stream.ReadByte();
							auto fmt = static_cast<BlockFormat>(fmtcode & 0xf);
							
							switch(fmt) {
								case BlockFormat::Constant:
									DecodeConstant(fmtcode, block);
									break;
								case BlockFormat::LinearX:
									DecodeLinear(fmtcode, block);
									break;
								case BlockFormat::LinearY:
									DecodeLinear(fmtcode, block);
									break;
								case BlockFormat::LinearZ:
									DecodeLinear(fmtcode, block);
									break;
								case BlockFormat::PlanarX:
									DecodePlanar(fmtcode, block);
									break;
								case BlockFormat::PlanarY:
									DecodePlanar(fmtcode, block);
									break;
								case BlockFormat::PlanarZ:
									DecodePlanar(fmtcode, block);
									break;
								case BlockFormat::Volumetric:
									DecodeVolumetric(fmtcode, block);
									break;
								default:
									SPRaise("Unsupported block format: 0x%02x",
											static_cast<int>(fmtcode));
							}
							
							// put color
							for(int xx = 0; xx < 8; xx++) {
								for(int yy = 0; yy < 8; yy++) {
									auto b = block.needscolor[xx][yy];
									if(!b) continue;
									for(int zz = 0; zz < 8; zz++) {
										if(!(b & (1<<zz))) continue;
										auto c = block.colors[xx][yy][zz];
										c &= 0xffffff;
										c |= 100 << 24; // health
										mp->colorMap[xx + x][yy + y][zz + z] = c;
									}
								}
							}
							
							// block done
						} // z
					} // x
				} // y
				
				return mp.Unmanage();
			}
			
		};
		
		GameMap *GameMap::LoadNGMap(IStream *zstream,
									std::function<void(float)> progressListener) {
			DeflateStream stream(zstream, CompressModeDecompress);
			return NGMapDecoder(stream).Decode(progressListener);
		}
		GameMap *GameMap::LoadNGMap(IStream *stream) {
			return LoadNGMap(stream, [](float){});
		}
		
#pragma mark - Encoder
		
		struct GameMap::ColorBlockEmitter {
			std::vector<uint8_t>& out;
			const NGMapOptions& opt;
			
			int diversityLimit;
			
			ColorBlockEmitter(std::vector<uint8_t>& out,
							  const NGMapOptions& opt):out(out),opt(opt) {
				diversityLimit = 34 - opt.quality / 3;
				diversityLimit *= diversityLimit;
			}
			
		private:
			
			void EncodeVolumetricRaw(ColorBlock& block) {
				out.push_back(MakeFormat(BlockFormat::Volumetric,
										 VolumetricBlockSubFormat::Raw));
				for(int x = 0; x < 8; x++)
					for(int yy = 0; yy < 8; yy++) {
						int y = (x & 1) ? (7 - yy) : yy;
						auto b = block.needscolor[x][y];
						if(!b) continue;
						
						for(int zz = 0; zz < 8; zz++) {
							int z = (yy & 1) ? (7 - zz) : zz;
							if(!(b&(1<<z))) continue;
							
							auto c = block.colors[x][y][z];
							auto b = IntVectorFromColor(c);
							out.push_back(static_cast<uint8_t>(b.x));
							out.push_back(static_cast<uint8_t>(b.y));
							out.push_back(static_cast<uint8_t>(b.z));
						}
					}
			}
			
			void TryEmitVolumetric(ColorBlock& block) {
				EncodeVolumetricRaw(block);
			}
			
			// fills holes for better compression
			void FillGap(PlanarColorBlock& sub, uint8_t needscolor[8]) {
				if(needscolor[0] == 0xff &&
				   needscolor[1] == 0xff &&
				   needscolor[2] == 0xff &&
				   needscolor[3] == 0xff &&
				   needscolor[4] == 0xff &&
				   needscolor[5] == 0xff &&
				   needscolor[6] == 0xff &&
				   needscolor[7] == 0xff) {
					// no gap to fill
					return;
				}
				
				struct Point {
					int8_t x, y;
					Point() = default;
					Point(int x, int y):x(x), y(y) {}
					static Point Bad() { return Point {-1, -1}; }
					bool IsBad() const { return x == -1; }
					inline int DistanceTo(Point o) const {
						int xx = x, yy = y;
						xx -= o.x; yy -= o.y;
						if(xx < 0) xx = -yy;
						if(yy < 0) yy = -yy;
						return xx + yy;
					}
					static Point Nearer(Point here, Point a, Point b) {
						if(a.IsBad()) return b;
						if(b.IsBad()) return a;
						auto d1 = here.DistanceTo(a);
						auto d2 = here.DistanceTo(b);
						return d1 < d2 ? a : b;
					}
				};
				
				Point nearest[8][8];
				
				Point tmpnear[8];
				Point oldnear[8];
				std::fill(tmpnear, tmpnear + 8, Point::Bad());
				for(auto*n:nearest) std::fill(n, n + 8, Point::Bad());
				for(int x = 0; x < 8; x++) {
					auto b = needscolor[x];
					std::copy(tmpnear, tmpnear + 8, oldnear);
					for(int y = 0; y < 8; y++) {
						if(b & (1 << y)) {
							tmpnear[y] = Point {x, y};
						} else {
							Point here {x, y};
							if(y == 0)
								tmpnear[y] = Point::Nearer(here, oldnear[y+1], oldnear[y]);
							else if(y == 7)
								tmpnear[y] = Point::Nearer(here, oldnear[y-1], oldnear[y]);
							else
								tmpnear[y] = Point::Nearer(here, Point::Nearer(here, oldnear[y-1], oldnear[y+1]), oldnear[y]);
						}
						nearest[x][y] = tmpnear[y];
					}
				}
				std::fill(tmpnear, tmpnear + 8, Point::Bad());
				for(int x = 7; x >= 0; x--) {
					auto b = needscolor[x];
					std::copy(tmpnear, tmpnear + 8, oldnear);
					for(int y = 0; y < 8; y++) {
						if(b & (1 << y)) {
							tmpnear[y] = Point {x, y};
						} else {
							Point here {x, y};
							if(y == 0)
								tmpnear[y] = Point::Nearer(here, oldnear[y+1], oldnear[y]);
							else if(y == 7)
								tmpnear[y] = Point::Nearer(here, oldnear[y-1], oldnear[y]);
							else
								tmpnear[y] = Point::Nearer(here, Point::Nearer(here, oldnear[y-1], oldnear[y+1]), oldnear[y]);
						}
						nearest[x][y] = Point::Nearer(Point{x,y}, tmpnear[y], nearest[x][y]);
					}
				}
				std::fill(tmpnear, tmpnear + 8, Point::Bad());
				for(int y = 0; y < 8; y++) {
					std::copy(tmpnear, tmpnear + 8, oldnear);
					for(int x = 0; x < 8; x++) {
						if(needscolor[x] & (1 << y)) {
							tmpnear[x] = Point {x, y};
						} else {
							Point here {x, y};
							if(x == 0)
								tmpnear[x] = Point::Nearer(here, oldnear[x+1], oldnear[x]);
							else if(x == 7)
								tmpnear[x] = Point::Nearer(here, oldnear[x-1], oldnear[x]);
							else
								tmpnear[x] = Point::Nearer(here, Point::Nearer(here, oldnear[x-1], oldnear[x+1]), oldnear[x]);
						}
						nearest[x][y] = Point::Nearer(Point{x,y}, tmpnear[x], nearest[x][y]);
					}
				}
				std::fill(tmpnear, tmpnear + 8, Point::Bad());
				for(int y = 7; y >= 0; y--) {
					std::copy(tmpnear, tmpnear + 8, oldnear);
					for(int x = 0; x < 8; x++) {
						if(needscolor[x] & (1 << y)) {
							tmpnear[x] = Point {x, y};
						} else {
							Point here {x, y};
							if(x == 0)
								tmpnear[x] = Point::Nearer(here, oldnear[x+1], oldnear[x]);
							else if(x == 7)
								tmpnear[x] = Point::Nearer(here, oldnear[x-1], oldnear[x]);
							else
								tmpnear[x] = Point::Nearer(here, Point::Nearer(here, oldnear[x-1], oldnear[x+1]), oldnear[x]);
						}
						nearest[x][y] = Point::Nearer(Point{x,y}, tmpnear[x], nearest[x][y]);
					}
				}
				for(int x = 0; x < 8; x++) {
					auto b = needscolor[x];
					for(int y = 0; y < 8; y++) {
						if(b & (1 << y)) continue;
						auto p = nearest[x][y];
						if(!p.IsBad()) {
							sub.colors[x][y] = sub.colors[p.x][p.y];
						}
					}
				}
			}
			
			void EmitCT(PlanarColorBlock& sub, ColorBlock& block, BlockFormat fmt) {
				out.push_back(MakeFormat(fmt,
										 PlanarBlockSubFormat::Lossy));
				
				ct::MacroBlock blockY, blockU, blockV;
				
				for(int x = 0; x < 8; x++)
					for(int y = 0; y < 8; y++){
						auto c = sub.colors[x][y];
						std::swap(c.z, c.x);
						int vv = c.z - c.x;
						int t = c.x - c.y + ((vv + 1) >> 1);
						int yy = c.y + (t >> 1);
						int uu = -t;
						blockY.pixels[x][y] = yy;
						blockU.pixels[x][y] = uu;
						blockV.pixels[x][y] = vv;
					}
				
				BitWriter writer(out);
				
				auto doBlock = [&](ct::MacroBlock& block) {
					ct::Encoder encoder(writer, opt);
					encoder.Encode(block);
				};
				
				doBlock(blockY);
				doBlock(blockU);
				doBlock(blockV);
				
			}
			
			static int CountNeedsColor(uint8_t *bits, std::size_t numRows) {
				int cnt = 0;
				static const uint8_t pc[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
				for(std::size_t i = 0; i < numRows; i++) {
					cnt += pc[bits[i] & 15] + pc[bits[i] >> 4];
				}
				return cnt;
			}
			
			void EncodePlanar(PlanarColorBlock& sub, ColorBlock& block, BlockFormat fmt) {
				uint8_t needscolor[8];
				switch(fmt) {
					case BlockFormat::PlanarX:
						block.GetNeedsColorMapPlanarX(needscolor);
						break;
					case BlockFormat::PlanarY:
						block.GetNeedsColorMapPlanarY(needscolor);
						break;
					case BlockFormat::PlanarZ:
						block.GetNeedsColorMapPlanarZ(needscolor);
						break;
					default:
						std::fill(needscolor, needscolor + 8, 0);
						break;
				}
				
				if(opt.quality < 100 && CountNeedsColor(needscolor, 8) > 5) {
					FillGap(sub, needscolor);
					EmitCT(sub, block, fmt);
				} else {
					out.push_back(MakeFormat(fmt,
											 PlanarBlockSubFormat::Raw));
					for(int x = 0; x < 8; x++)
						for(int yy = 0; yy < 8; yy++) {
							int y = (x & 1) ? (7 - yy) : yy;
							if(!(needscolor[x] & (1 << y))) continue;
							auto b = sub.colors[x][y];
							out.push_back(static_cast<uint8_t>(b.x));
							out.push_back(static_cast<uint8_t>(b.y));
							out.push_back(static_cast<uint8_t>(b.z));
						}
				}
			}
			
			void TryEmitPlanar(ColorBlock& block) {
				int xx = block.ComputeDiversity2D_X();
				int yy = block.ComputeDiversity2D_Y();
				int zz = block.ComputeDiversity2D_Z();
				int mn = std::min(xx, std::min(yy, zz));
				if(mn <= diversityLimit) {
					PlanarColorBlock sub;
					if(xx == mn) {
						block.ToPlanarX(sub);
						EncodePlanar(sub, block, BlockFormat::PlanarX);
					}else if(yy == mn) {
						block.ToPlanarY(sub);
						EncodePlanar(sub, block, BlockFormat::PlanarY);
					}else{
						block.ToPlanarZ(sub);
						EncodePlanar(sub, block, BlockFormat::PlanarZ);
					}
				}else{
					TryEmitVolumetric(block);
				}
				
			}
			
			struct LinearLinearEncoded {
				IntVector3 col1, col2;
				int error;
			};
			
			LinearLinearEncoded TryEncodeLinearLinear(LinearColorBlock& sub, uint8_t needscolor) {
				LinearLinearEncoded encoded;
				IntVector3 avg(0, 0, 0);
				int numcolors = 0;
				for(int i = 0; i < 8; i++) {
					if(needscolor & (1 << i)) {
						avg += sub.colors[i];
						numcolors++;
					}
				}
				if(numcolors > 1) avg /= numcolors;
				
				IntVector3 coef(0, 0, 0);
				static const int8_t weights[8] = {-7, -5, -3, -1, 1, 3, 5, 7};
				int factor = 0;
				for(int i = 0; i < 8; i++) {
					if(needscolor & (1 << i)) {
						coef += (sub.colors[i] - avg) * weights[i];
						factor += weights[i] * weights[i];
					}
				}
				coef *= 7;
				coef /= factor;
			
				encoded.col1 = avg - coef;
				encoded.col2 = avg + coef;
				
				encoded.col1.x = std::max(0, encoded.col1.x);
				encoded.col1.y = std::max(0, encoded.col1.y);
				encoded.col1.z = std::max(0, encoded.col1.z);
				encoded.col1.x = std::min(255, encoded.col1.x);
				encoded.col1.y = std::min(255, encoded.col1.y);
				encoded.col1.z = std::min(255, encoded.col1.z);
				
				encoded.col2.x = std::max(0, encoded.col2.x);
				encoded.col2.y = std::max(0, encoded.col2.y);
				encoded.col2.z = std::max(0, encoded.col2.z);
				encoded.col2.x = std::min(255, encoded.col2.x);
				encoded.col2.y = std::min(255, encoded.col2.y);
				encoded.col2.z = std::min(255, encoded.col2.z);

				// try encoding to predict error
				LinearColorBlock pred;
				NGMapDecoder::DecodeLinearLinear(encoded.col1, encoded.col2, pred);
				
				encoded.error = 0;
				for(int i = 0; i < 8; i++) {
					if(needscolor & (1 << i)) {
						auto diff = sub.colors[i] - pred.colors[i];
						encoded.error = std::max(encoded.error, IntVector3::Dot(diff, diff));
					}
				}

				return encoded;
			}
			
			void EncodeLinear(LinearColorBlock& sub, ColorBlock& block, BlockFormat fmt) {
				
				uint8_t needscolor = 0;
				switch(fmt) {
					case BlockFormat::LinearX:
						needscolor = block.GetNeedsColorMapLinearX();
						break;
					case BlockFormat::LinearY:
						needscolor = block.GetNeedsColorMapLinearY();
						break;
					case BlockFormat::LinearZ:
						needscolor = block.GetNeedsColorMapLinearZ();
						break;
					default:
						// shouldn't happen...
						break;
				}
				
				LinearLinearEncoded lin = TryEncodeLinearLinear(sub, needscolor);
				
				if(lin.error < diversityLimit) {
					out.push_back(MakeFormat(fmt,
											 LinearBlockSubFormat::Linear));
					
					out.push_back(static_cast<uint8_t>(lin.col1.x));
					out.push_back(static_cast<uint8_t>(lin.col1.y));
					out.push_back(static_cast<uint8_t>(lin.col1.z));
					
					out.push_back(static_cast<uint8_t>(lin.col2.x));
					out.push_back(static_cast<uint8_t>(lin.col2.y));
					out.push_back(static_cast<uint8_t>(lin.col2.z));
					
				} else{
					out.push_back(MakeFormat(fmt,
											 LinearBlockSubFormat::Raw));
					for(int x = 0; x < 8; x++){
						if(!(needscolor&(1<<x))) continue;
						auto b = sub.colors[x];
						out.push_back(static_cast<uint8_t>(b.x));
						out.push_back(static_cast<uint8_t>(b.y));
						out.push_back(static_cast<uint8_t>(b.z));
					}
				}
				
				
			}
			
			void TryEmitLinear(ColorBlock& block) {
				int xx = block.ComputeDiversity1D_X();
				int yy = block.ComputeDiversity1D_Y();
				int zz = block.ComputeDiversity1D_Z();
				int mn = std::min(xx, std::min(yy, zz));
				if(mn * 3 <= diversityLimit) {
					LinearColorBlock sub;
					if(xx == mn) {
						block.ToLinearX(sub);
						EncodeLinear(sub, block, BlockFormat::LinearX);
					}else if(yy == mn) {
						block.ToLinearY(sub);
						EncodeLinear(sub, block, BlockFormat::LinearY);
					}else{
						block.ToLinearZ(sub);
						EncodeLinear(sub, block, BlockFormat::LinearZ);
					}
				}else{
					TryEmitPlanar(block);
				}
			}
			
		public:
			
			void TryEmitConstant(ColorBlock& block) {
				// constant mode looks super bad, so raise the threshold
				if(block.ComputeDiversity() * 5 <= diversityLimit) {
					auto cst = block.ToConstant();
					out.push_back(MakeFormat(BlockFormat::Constant,
											 ConstantBlockSubFormat::Constant));
					out.push_back(static_cast<uint8_t>(cst.x));
					out.push_back(static_cast<uint8_t>(cst.y));
					out.push_back(static_cast<uint8_t>(cst.z));
				} else {
					TryEmitLinear(block);
				}
			}
			
		};
		
		
		
		void GameMap::SaveNGMap(IStream *zstream, const NGMapOptions& opt) {
			opt.Validate();
			
			DeflateStream stream(zstream, CompressModeCompress);
			
			int w = Width();
			int h = Height();
			int d = Depth();
			
			stream.WriteLittleInt(signature);
			
			stream.WriteLittleShort(static_cast<uint16_t>(w));
			stream.WriteLittleShort(static_cast<uint16_t>(h));
			stream.WriteLittleShort(static_cast<uint16_t>(d));
			
			if(d > 64) {
				// due to bit width of solidMap
				SPRaise("Depth over 64 is not supported: %d", d);
			}
			
			stream.WriteByte(opt.quality);
			
			// write geometry
			std::vector<uint8_t> columnbuf;
			columnbuf.reserve(256);
			for(int y = 0; y < h; y++) {
				bool odd = y & 1;
				for(int i = 0, x = odd ? w - 1 : 0, s = odd ? -1 : 1; i < w; i++, x += s) {
					
					// output column
					uint64_t solidMap = GetSolidMapWrapped(x, y);
					// first, try bitmap format
					{
						columnbuf.clear();
						int z = 1;
						bool last = (solidMap & 1) != 0;
						int step = 0;
						while(z < d) {
							bool b = ((solidMap >> z) & 1) != 0;
							if(b != last) {
								uint8_t outb = static_cast<uint8_t>(z);
								if(step == 0) {
									step = 1;
									if(last) outb |= 0x80;
								}
								columnbuf.push_back(outb);
								last = b;
							}
							z++;
						}
						
						{
							uint8_t outb = static_cast<uint8_t>(z);
							if(step == 0) {
								step = 1;
								if(last) outb |= 0x80;
							}
							columnbuf.push_back(outb);
						}
					}
					// second, estimate size of bitmap format
					std::size_t bmpfmtsize = 0;
					{
						for(int z = 0; z < d; z += 16) {
							if((solidMap >> z) & 0xffff) {
								bmpfmtsize+=2;
							}
						}
					}
					
					// compare and choose the smaller one
					if(bmpfmtsize < columnbuf.size() &&
					   bmpfmtsize != 0) {
						// bitmap format is smaller
						int fmt = static_cast<int>(ColumnFormat::Bitmap);
						for(int z = 0; z < d; z += 16) {
							if((solidMap >> z) & 0xffff) {
								fmt |= 1 << (1 + (z >> 4));
							}
						}
						stream.WriteByte(fmt);
						for(int z = 0; z < d; z += 16) {
							if((solidMap >> z) & 0xffff) {
								stream.WriteLittleShort(static_cast<uint16_t>(solidMap >> z));
							}
						}
					}else{
						// rle format is smaller
						stream.WriteByte(static_cast<int>(ColumnFormat::Rle));
						stream.Write(columnbuf.data(), columnbuf.size());
					}
					
					
					// one column done
				}
			}
			
			// write colors
			std::vector<uint8_t> blockbuf;
			blockbuf.reserve(1024);
			
			ColorBlockEmitter emitter(blockbuf, opt);
			
			for(int y = 0; y < h; y += 8) {
				for(int x = 0; x < w; x += 8) {
					for(int zz = 0; zz < d; zz += 8) {
						int z = (x & 8) ? (((d+7)&~7) - 8 - zz) : zz;
						
						ColorBlock block;
						ComputeNeedsColor(x, y, z, block.needscolor);
						
						// acquire colors
						for(int xx = 0; xx < 8; xx++) {
							for(int yy = 0; yy < 8; yy++) {
								auto bits = block.needscolor[xx][yy];
								if(bits) {
									for(int zz = 0; zz < 8; zz++) {
										if(bits & (1 << zz)) {
											block.colors[xx][yy][zz] = GetColor(xx+x, yy+y, zz+z);
										}
									}
								}
							}
						}
						
						// decide format
						blockbuf.clear();
						emitter.TryEmitConstant(block);
						
						
						stream.Write(blockbuf.data(), blockbuf.size());
						
						
						// block done
					} // z
				} // x
			} // y
			
			stream.DeflateEnd();
			
		}
		
		/** Computes which blocks are visible and therefore needs to be colored, in
		 * x <= bx < x + 8, y <= by < y + 8, z <= bz < z + 8. */
		void GameMap::ComputeNeedsColor(int x, int y, int z, uint8_t needscolor[8][8]) {
			uint16_t subsolidmap[10][10]; // part of solid map
			uint16_t earth = 0;
			if(z + 8 >= Depth()) {
				int b = Depth() - z;
				if(b <= 0) earth = 0xffff;
				else if(b < 16) {
					earth = static_cast<uint16_t>(~((1<<b)-1));
				}
			}
			for(int cx = 0; cx < 10; cx++) {
				for(int cy = 0; cy < 10; cy++) {
					int xx = cx + x - 1, yy = cy + y - 1;
					auto m = (xx >= 0 && yy >= 0 && xx < Width() && yy < Height()) ?
					solidMap[xx][yy] : 0;
					m |= earth;
					if(z == 0) {
						m <<= 1;
					}else{
						m >>= z - 1;
					}
					subsolidmap[cx][cy] = static_cast<uint16_t>(m & 0x3ff) | earth;
				}
			}
			
			// compute which cell have to be colored
			for(int cx = 0; cx < 8; cx++) {
				for(int cy = 0; cy < 8; cy++) {
					auto mask = subsolidmap[cx + 1][cy + 1];
					auto b = mask;
					auto result = 0;
					result = b & ~subsolidmap[cx][cy + 1];
					result |= b & ~subsolidmap[cx + 2][cy + 1];
					result |= b & ~subsolidmap[cx + 1][cy];
					result |= b & ~subsolidmap[cx + 1][cy + 2];
					result |= b & ~(b >> 1);
					result |= b & ~(b << 1);
					needscolor[cx][cy] = static_cast<uint8_t>(result >> 1);
				}
			}
		}
		
	}
}