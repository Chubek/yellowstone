#include "../common/domterm_pager.hpp"
#include "../common/cli.hpp"
#include "../qbfd/qBFD.hpp"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <unistd.h>

using namespace qbfd;
static const char* arch_name(Arch a) { switch(a){case Arch::X86:return "i386";case Arch::X86_64:return "x86-64";case Arch::ARM:return "arm";case Arch::AArch64:return "aarch64";case Arch::RISCV32:return "riscv32";case Arch::RISCV64:return "riscv64";case Arch::PowerPC:return "ppc";case Arch::PowerPC64:return "ppc64";case Arch::MIPS:return "mips";case Arch::MIPS64:return "mips64";default:return "unknown";} }
int main(int argc, char** argv) {
  bool tui=false, color=isatty(STDOUT_FILENO), no_text=false;
  std::string path;
  for (int i=1;i<argc;++i) { std::string a=argv[i]; if(a=="--tui"||a=="--pager") tui=true; else if(a=="--no-color") color=false; else if(a=="--no-pager") tui=false; else if(a=="--no-text") no_text=true; else if(a=="-h"||a=="--help") { std::cout<<"usage: "<<argv[0]<<" [--tui] [--no-color] FILE\n"; return 0; } else if(path.empty()) path=a; }
  if(path.empty()){std::cerr<<"missing input\n";return 2;}
  auto opened=open(path); if(!opened){std::cerr<<argv[0]<<": "<<opened.error().message<<"\n";return 1;}
  const auto& f=*opened->object; std::ostringstream out;
  auto c=[&](const char* code){return color?code:"";};
  out<<c("\033[1;36m")<<argv[0]<<c("\033[0m")<<": "<<path<<"\n";
  out<<"Format: "<<qobj::formatName(f.format())<<"  Architecture: "<<arch_name(f.arch())<<"  Bits: "<<(f.is64Bit()?64:32)<<"\n";
  out<<"Entry: 0x"<<std::hex<<f.entryPoint()<<std::dec<<"\n\n"<<c("\033[1m")<<"Sections"<<c("\033[0m")<<"\n";
  out<<"Idx  Name                         Size       VMA          Offset Flags\n";
  for(const auto&s:f.sections()) if(s.index||!s.name.empty()) out<<s.index<<"    "<<s.name<<std::string(s.name.size()<27?27-s.name.size():1,' ')<<std::setw(8)<<s.size<<"  0x"<<std::hex<<std::setw(10)<<s.vma<<"  0x"<<std::setw(8)<<s.fileOffset<<std::dec<<" "<<qobj::sectionFlags(s)<<"\n";
  if(!f.symbols().empty()){out<<"\n"<<c("\033[1m")<<"Symbols"<<c("\033[0m")<<"\n";for(const auto&s:f.symbols())out<<"0x"<<std::hex<<s.value<<std::dec<<" "<<s.name<<"\n";}
  std::string text=out.str(); if(tui){qobj::DomtermPager p;if(p.run(text))return 0;} std::cout<<text; return 0;
}
