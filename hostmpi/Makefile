# Native (macOS/Linux) build of the xv6 MPI subset, the xv6 PETSc subset and
# the fem shell programs, so a host can run ranks of the same MPI job as an
# xv6 board or QEMU guest (see doc/readme/readme_mpi_tcp.md).
#
#   make -C hostmpi            -> build/hostmpi/{shell_mpi_benchmark,mpitest,bar_mpi_trace}
#
# Uses the system C++ library and real Eigen (Homebrew: brew install eigen).

R      := ..
U      := $(R)/user
OUT    := $(R)/build/hostmpi
CC     ?= cc
CXX    ?= c++
EIGEN  ?= $(firstword $(wildcard /opt/homebrew/include/eigen3 /usr/local/include/eigen3 /usr/include/eigen3))
OPT    ?= -O3 -mcpu=native
CFLAGS   = $(OPT) -g -Wall -DMPI_HOST -I$(U)
CXXFLAGS = -std=c++17 $(OPT) -g -Wall -Wno-unused-variable -DMPI_HOST -DFEAPPV_HAS_PETSC \
	-I$(OUT)/include -I$(U)/petsc/include -I$(U)/fem -I$(U)/fem/include -I$(EIGEN)

FEM_SRCS = $(addprefix $(U)/fem/,elements/solid1d/bar1.cpp elements/frame/truss2.cpp \
	elements/frame/beam2.cpp elements/frame/beam3.cpp elements/solid2d/continuum.cpp \
	elements/shells/plate_q4.cpp elements/shells/shell_q4.cpp program/model.cpp \
	program/spatial_moment.cpp program/petsc_model.cpp)
LIB_OBJS = $(patsubst $(U)/%.cpp,$(OUT)/%.o,$(FEM_SRCS) $(U)/petsc/petsc.cpp)
# MPI is a shared library, as with Open MPI: on macOS the profiling wrappers
# in tests/shell_mpi_profile.cpp interpose dynamic-library imports.
MPILIB = $(OUT)/libxmpi.dylib
LINK_MPI = -L$(OUT) -lxmpi -Wl,-rpath,@executable_path

PROGS = $(OUT)/shell_mpi_benchmark $(OUT)/shell_serial_benchmark $(OUT)/bar_mpi_trace $(OUT)/mpitest

all: $(PROGS)

$(OUT)/include/mpi.h: $(U)/include/mpi.h
	@mkdir -p $(dir $@)
	cp $< $@

$(OUT)/mpi.o: $(U)/mpi.c $(U)/mpi_host.h $(U)/include/mpi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OUT)/mpi_host_alias.o: $(U)/mpi_host_alias.c $(U)/include/mpi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/%.o: $(U)/%.cpp $(OUT)/include/mpi.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MD -c -o $@ $<

$(MPILIB): $(OUT)/mpi.o $(OUT)/mpi_host_alias.o
	$(CC) -dynamiclib -install_name @rpath/libxmpi.dylib -o $@ $^

$(OUT)/libhostfem.a: $(LIB_OBJS)
	rm -f $@
	ar rcs $@ $^

# Like fem's CMake build: the profiling wrappers are a dylib so that their
# __interpose entries apply to the executable's MPI imports.
$(OUT)/libshell_mpi_profile.dylib: $(OUT)/fem/tests/shell_mpi_profile.o $(MPILIB)
	$(CXX) -dynamiclib -install_name @rpath/libshell_mpi_profile.dylib -o $@ $< $(LINK_MPI)
$(OUT)/shell_mpi_benchmark: $(OUT)/fem/tests/shell_mpi.o $(OUT)/libhostfem.a $(OUT)/libshell_mpi_profile.dylib $(MPILIB)
	$(CXX) -o $@ $(filter %.o %.a,$^) -lshell_mpi_profile $(LINK_MPI)
$(OUT)/shell_serial_benchmark: $(OUT)/fem/tests/shell_serial.o $(OUT)/libhostfem.a $(MPILIB)
	$(CXX) -o $@ $(filter %.o %.a,$^) $(LINK_MPI)
$(OUT)/bar_mpi_trace: $(OUT)/fem/tests/bar_mpi_trace.o $(OUT)/libhostfem.a $(MPILIB)
	$(CXX) -o $@ $(filter %.o %.a,$^) $(LINK_MPI)
$(OUT)/mpitest: $(U)/mpitest.c $(MPILIB)
	$(CC) $(CFLAGS) -o $@ $< $(LINK_MPI)

clean:
	rm -rf $(OUT)

-include $(wildcard $(OUT)/*/*.d $(OUT)/*/*/*.d $(OUT)/*/*/*/*.d)
.PHONY: all clean
