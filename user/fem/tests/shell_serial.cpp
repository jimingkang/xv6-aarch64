#include "shell_case.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <sys/resource.h>

int main(int argc, char** argv) {
    try {
        if (argc < 4 || argc > 6) throw std::runtime_error("Usage: shell_serial_benchmark nx ny reference|- [repetitions] [write_reference]");
        const int nx=std::stoi(argv[1]), ny=std::stoi(argv[2]);
        const int repetitions=argc>=5 ? std::stoi(argv[4]) : 3;
        const bool has_reference=std::string(argv[3])!="-";
        if (nx<1 || ny<1 || repetitions<1) throw std::runtime_error("Invalid grid or repetitions");
        auto shell=shell_test::cantilever(nx,ny);
        shell.model.set_element_threads(1);
        std::ifstream input(argv[3]);
        std::size_t n=0;
        if (has_reference && (!(input>>n) || n!=shell.load.size())) throw std::runtime_error("Invalid reference");
        if (!has_reference) n=shell.load.size();
        struct State { fem::Vector u, reaction; };
        std::array<State,4> reference;
        std::array<State,4> written_reference;
        if (has_reference) for (auto& state : reference) {
            state.u.resize(n); state.reaction.resize(n);
            for (auto& value : state.u) input>>value;
            for (auto& value : state.reaction) input>>value;
        }
        if (has_reference && !input) throw std::runtime_error("Incomplete reference");
        struct Sample { double wall; fem::SolveTimings timing; };
        std::vector<Sample> samples(repetitions);
        std::cout << "SERIAL_GRID nodes " << (nx+1)*(ny+1) << " elements " << nx*ny
                  << " free_dofs " << 6*nx*(ny+1) << " repetitions " << repetitions << '\n' << std::flush;
        for (auto& sample : samples) {
            fem::Vector u(n), load(n);
            double u_error=0, reaction_error=0, support=0, free_residual=0;
            const auto start=std::chrono::steady_clock::now();
            for (int step=0; step<4; ++step) {
                for (std::size_t i=0;i<n;++i) load[i]=shell.load[i]*(step+1)/4;
                const auto reaction=shell.model.solve(u,load,1e-8,&sample.timing);
                if(argc==6) written_reference[step]={u,reaction};
                if (has_reference) for (std::size_t i=0;i<n;++i) {
                    u_error=std::max(u_error,std::abs(u[i]-reference[step].u[i]));
                    reaction_error=std::max(reaction_error,std::abs(reaction[i]-reference[step].reaction[i]));
                }
                support=0;
                for(int j=0;j<=ny;++j) support+=reaction[6*std::size_t(j*(nx+1))+2];
                if (std::abs(support-0.5*(step+1)/4)>1e-6)
                    throw std::runtime_error("Support reaction balance failed");
                double step_residual=0;
                for(int j=0;j<=ny;++j) for(int i=1;i<=nx;++i) for(int k=0;k<6;++k) {
                    const auto dof=6*std::size_t(j*(nx+1)+i)+k;
                    if (!std::isfinite(u[dof]) || !std::isfinite(reaction[dof]))
                        throw std::runtime_error("Non-finite state");
                    step_residual=std::max(step_residual,std::abs(reaction[dof]));
                }
                if(step_residual>1e-7) throw std::runtime_error("Free residual check failed");
                free_residual=std::max(free_residual,step_residual);
                if (!has_reference) std::cout<<std::setprecision(12)<<"SERIAL_STEP "<<step+1
                    <<" elapsed "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()
                    <<" tip "<<u[shell.tip]<<" support "<<support
                    <<" free_residual_inf "<<step_residual<<'\n'<<std::flush;
            }
            sample.wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            if(u_error>=1e-8 || reaction_error>=1e-7) throw std::runtime_error("Reference mismatch");
            if(has_reference) std::cout<<std::setprecision(12)<<"SERIAL_CHECK "<<nx<<' '<<ny<<' '<<u_error<<' '
                     <<reaction_error<<' '<<u[shell.tip]<<' '<<support<<'\n'<<std::flush;
            else std::cout<<"SERIAL_BALANCE_CHECK "<<nx<<' '<<ny<<' '<<free_residual<<' '
                          <<u[shell.tip]<<' '<<support<<'\n'<<std::flush;
        }
        std::sort(samples.begin(),samples.end(),[](const auto& a,const auto& b){return a.wall<b.wall;});
        const auto& s=samples[samples.size()/2];
        struct rusage usage{};
        if(getrusage(RUSAGE_SELF,&usage)) throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
        const double peak=double(usage.ru_maxrss)/(1024*1024);
#else
        const double peak=double(usage.ru_maxrss)/1024;
#endif
        std::cout<<"SERIAL_TIMING "<<nx<<' '<<ny<<' '<<s.wall<<' '<<s.timing.element_seconds<<' '
                 <<s.timing.assembly_seconds<<' '<<s.timing.linear_seconds<<' '<<peak<<' '
                 <<s.timing.assemblies<<' '<<s.timing.linear_solves<<'\n';
        if(argc==6) {
            std::ofstream output(argv[5]);
            output<<n<<'\n'<<std::setprecision(17);
            for(const auto& state:written_reference) {
                for(auto value:state.u) output<<value<<' ';
                for(auto value:state.reaction) output<<value<<' ';
                output<<'\n';
            }
            if(!output) throw std::runtime_error("Cannot write reference");
        }
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
