#include "frame_engine.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <cmath>

static std::vector<uint8_t> read(const std::string& path,size_t limit) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f || f.tellg()<0 || uint64_t(f.tellg())>limit) throw std::runtime_error("oracle input bounds");
    std::vector<uint8_t> bytes(size_t(f.tellg()));f.seekg(0);
    if(!bytes.empty()&&!f.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))throw std::runtime_error("oracle input read");
    return bytes;
}
int main(int argc,char** argv) {
    try {
        if(argc!=9)throw std::runtime_error("oracle modeldir left.raw right.raw width height alpha output.raw threads");
        auto param=read(std::string(argv[1])+"/flownet.param",1024*1024),weights=read(std::string(argv[1])+"/flownet.bin",32*1024*1024);
        const int w=std::stoi(argv[4]),h=std::stoi(argv[5]);const float alpha=std::stof(argv[6]);
        if(w<=0||h<=0||w>4096||h>4096||uint64_t(w)*h>4096u*2160u||!std::isfinite(alpha)||alpha<0||alpha>1)throw std::runtime_error("oracle shape/alpha");
        auto a=read(argv[2],128*1024*1024),b=read(argv[3],128*1024*1024);
        if(a.size()!=uint64_t(w)*h*3||b.size()!=a.size())throw std::runtime_error("oracle image sizes");
        innocent::interpolation::Options options;options.backend=innocent::interpolation::Backend::Cpu;
        options.threads=std::stoi(argv[8]);options.maxPixels=size_t(w)*h;
        innocent::interpolation::Status status;
        auto engine=innocent::interpolation::Engine::create(options,&status);
        if(!engine||!engine->configure(false,1,false).ok()||!engine->load({param.data(),param.size(),weights.data(),weights.size()}).ok())throw std::runtime_error("oracle model load");
        std::vector<uint8_t> output(a.size());
        status=engine->processRgb8({a.data(),w,h,size_t(w)*3,a.size()},{b.data(),w,h,size_t(w)*3,b.size()},alpha,
            {output.data(),w,h,size_t(w)*3,output.size()});
        if(!status.ok())throw std::runtime_error(std::string("oracle inference ")+status.name());
        std::ofstream f(argv[7],std::ios::binary);f.write(reinterpret_cast<const char*>(output.data()),std::streamsize(output.size()));
        if(!f)throw std::runtime_error("oracle output");
        std::cout<<"old CPU engine successful, bytes="<<output.size()<<'\n';return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
