#include "../firmware/src/ota_format.h"
#include <cassert>
#include <vector>
#include <fstream>
#include <iterator>
int main(int argc,char**argv){
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);
    std::vector<uint8_t> bundle((std::istreambuf_iterator<char>(file)),{});
    otaformat::Metadata meta;assert(bundle.size()>176);
    assert(otaformat::parse(bundle.data(),176,99,meta));assert(meta.sequence==100&&meta.size==256);
    assert(!otaformat::parse(bundle.data(),176,100,meta));
    assert(!otaformat::parse(bundle.data(),175,99,meta));
    for(int position:{0,8,39,46,47,111}){
        auto corrupt=bundle;corrupt[position]=0xff;
        assert(!otaformat::parse(corrupt.data(),176,99,meta));
    }
    auto padding=bundle;padding[81]=0;padding[82]='x';assert(!otaformat::parse(padding.data(),176,99,meta));
    assert(otaformat::equal(bundle.data(),bundle.data(),bundle.size()));
    auto changed=bundle;changed.back()^=1;assert(!otaformat::equal(bundle.data(),changed.data(),bundle.size()));
}
