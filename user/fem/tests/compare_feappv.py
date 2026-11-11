"""Run the unmodified FEAPpv reference executable against the C++ benchmarks."""
import argparse
import math
import re
import subprocess
import tempfile
from pathlib import Path


STEPS = 20
BEAM_ELEMENTS = 16


def solution(node):
    return f"""END

BATCh
  FORM,LONG
  DT,,{1 / STEPS}
  PROP,,1
  TOL,,1e-14
  LOOP,time,{STEPS}
    TIME
    LOOP,newton,25
      TANG,,1
    NEXT,newton
    DISP,,{node}
  NEXT,time
END
1 1 0 2 0 1 0 0

STOP
"""


def beam_deck():
    n = BEAM_ELEMENTS
    coordinates = "\n".join(f"{i + 1} 0 {2*i/n} 0" for i in range(n + 1))
    elements = "\n".join(f"{i + 1} 0 1 {i + 1} {i + 2}" for i in range(n))
    return f"""FEAP finite-rotation cantilever
{n + 1} {n} 1 2 3 2

COORdinates
{coordinates}

ELEMent
{elements}

BOUNdary
1 0 1 1 1

FORCe
{n + 1} 0 0 10 0

MATErial,1
FRAME
ELASTIC,,1e7,0.3
CROSS,,0.01,1e-6,1e-6
FINITE

""" + solution(n + 1)


def shell_deck():
    return """FEAP finite axisymmetric cylindrical shell
2 1 1 2 3 2

COORdinates
1 0 1 0
2 0 1 1

ELEMent
1 0 1 1 2

BOUNdary
1 0 0 1 1
2 0 0 1 1

FORCe
1 0 1408 0 0
2 0 1408 0 0

MATErial,1
SHELL
ELASTIC,,1e6,0.25
THICKNESS,,0.01
FINITE

""" + solution(2)


def linear_strip_deck():
    coordinates = "\n".join(f"{2*i+j+1} 0 {i/8} {j} 0"
                            for i in range(9) for j in range(2))
    elements = "\n".join(f"{i+1} 0 1 {2*i+1} {2*i+3} {2*i+4} {2*i+2}"
                         for i in range(8))
    return f"""FEAP small displacement shell strip under end moment
18 8 1 3 6 4

COORdinates
{coordinates}

ELEMent
{elements}

BOUNdary
1 0 1 1 1 1 1 1
2 0 1 1 1 1 1 1

FORCe
17 0 0 0 0 0 0.5 0
18 0 0 0 0 0 0.5 0

MATErial,1
SHELL
ELASTIC,,1.2e7,0
THICKNESS,,0.01
SMALL

""" + solution(17)


def run_reference(executable, directory, name, deck, node):
    (directory / f"I{name}").write_text(deck)
    result = subprocess.run(
        [executable, f"-iI{name}"], cwd=directory, input="", text=True,
        capture_output=True, timeout=60, check=True,
    )
    report = (directory / f"O{name}").read_text()
    if re.search(r"NO CONVERGENCE|\*ERROR\*", report + result.stdout, re.IGNORECASE):
        raise RuntimeError(f"FEAPpv reported an error for {name}")
    values = []
    for block in report.split("N o d a l   D i s p l a c e m e n t s")[1:]:
        lines = block.splitlines()
        for i, line in enumerate(lines[:8]):
            fields = line.split()
            if not fields or fields[0] != str(node):
                continue
            if len(fields) == 6:  # 2D coordinates and 3 DOFs on one line.
                values.append(tuple(float(x) for x in fields[3:]))
            elif len(fields) == 4 and len(lines[i+1].split()) == 6:
                values.append(tuple(float(x) for x in lines[i+1].split()))
            break
    if len(values) != STEPS:
        raise RuntimeError(f"Expected {STEPS} FEAPpv records for {name}, found {len(values)}")
    return values


def simo_planar_tip(panels):
    """Shoot the planar Simo–Reissner equilibrium ODE for the final dead load."""
    length, force, ea, ei = 2.0, 10.0, 1e5, 10.0
    kga = (5 / 6) * ea / (2 * (1 + 0.3))
    h = -length / panels

    def derivative(state):
        _, _, theta, moment = state
        c, s = math.cos(theta), math.sin(theta)
        stretch, shear = 1 + force * s / ea, force * c / kga
        dx = c * stretch - s * shear
        return dx, s * stretch + c * shear, moment / ei, -force * dx

    def advance(state, slope, increment):
        return tuple(x + increment * k for x, k in zip(state, slope))

    def integrate_back(tip_angle):
        state = (0.0, 0.0, tip_angle, 0.0)
        for _ in range(panels):
            k1 = derivative(state)
            k2 = derivative(advance(state, k1, h / 2))
            k3 = derivative(advance(state, k2, h / 2))
            k4 = derivative(advance(state, k3, h))
            state = tuple(x + h * (a + 2*b + 2*c + d) / 6
                          for x, a, b, c, d in zip(state, k1, k2, k3, k4))
        return state

    # Zero end moment is imposed at the tip. Shoot for zero rotation at the clamp.
    low, high = 0.0, math.pi / 2
    if not integrate_back(low)[2] < 0 < integrate_back(high)[2]:
        raise RuntimeError("Planar beam shooting root is not bracketed")
    for _ in range(45):
        angle = (low + high) / 2
        x, y, clamp_angle, _ = integrate_back(angle)
        if clamp_angle < 0:
            low = angle
        else:
            high = angle
    return -x - length, -y, angle


def run_comparison(reference, cpp, directory):
    beam = run_reference(reference, directory, "beam", beam_deck(), BEAM_ELEMENTS + 1)
    shell = run_reference(reference, directory, "shell", shell_deck(), 2)
    records = subprocess.check_output([cpp], text=True, timeout=60).splitlines()
    cpp_beam, cpp_ad, cpp_shell = {}, {}, {}
    refined = None
    for line in records:
        fields = line.split()
        if fields[:1] == ["BEAM"]:
            cpp_beam[int(fields[1])] = tuple(map(float, fields[2:]))
        if fields[:1] == ["BEAM_AD"]:
            cpp_ad[int(fields[1])] = tuple(map(float, fields[2:]))
        if fields[:1] == ["BEAM_REFINED"]:
            refined = tuple(map(float, fields[1:]))
        if fields[:1] == ["SHELL"]:
            cpp_shell[int(fields[1]), int(fields[2])] = float(fields[3])

    def history_error(a, b):
        return tuple(max(abs(a[i][j] - b[i][j]) for i in range(1, STEPS+1))
                     for j in range(3))

    print("Beam final (ux, uy, rz), FEAPpv:", beam[-1])
    print("  C++ explicit:", cpp_beam[STEPS], "C++ AD:", cpp_ad[STEPS])
    ad_error = history_error(cpp_beam, cpp_ad)
    beam_error = history_error(cpp_beam, dict(enumerate(beam, start=1)))
    print("  Max errors over all steps vs AD:", ad_error, "vs FEAPpv:", beam_error)
    if max(ad_error) > 1e-10 or max(beam_error) > 2e-7:
        raise RuntimeError("Finite beam does not match FEAPpv")

    continuum = simo_planar_tip(512)
    if max(abs(a-b) for a, b in zip(continuum, simo_planar_tip(256))) > 1e-9:
        raise RuntimeError("Planar continuum reference is not converged")
    if refined is None:
        raise RuntimeError("Missing refined C++ beam result")
    errors = [tuple(abs(a-b) for a, b in zip(tip, continuum))
              for tip in (cpp_beam[STEPS], refined)]
    print("Planar Simo–Reissner continuum final:", continuum)
    print("  16-element errors:", errors[0], "32-element errors:", errors[1])
    if any(fine >= coarse / 3.5 for coarse, fine in zip(*errors)):
        raise RuntimeError("Beam mesh refinement did not approach the continuum reference")
    if abs(shell[-1][0] - 0.2) > 1e-8:
        raise RuntimeError("FEAPpv shell does not match the axisymmetric equilibrium")
    print("Shell final radial displacement, FEAPpv:", shell[-1][0])
    previous = math.inf
    for n in (8, 16, 32, 64):
        error = max(abs(cpp_shell[n, i] - shell[i-1][0]) for i in range(1, STEPS+1))
        print(f"  {n:2d} facets  C++ final {cpp_shell[n, STEPS]:.10f}  max error {error:.4e}")
        if error >= previous / 3.8:
            raise RuntimeError("Faceted shell did not converge at the expected rate")
        previous = error
    if previous > 1e-4:
        raise RuntimeError("Finest faceted shell disagrees with FEAPpv")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("feappv", type=Path, help="Path to the original FEAPpv executable")
    parser.add_argument("cpp", type=Path, help="Path to finite_element_benchmarks")
    parser.add_argument("--shell-series", action="store_true",
                        help="Export axisymmetric and small-displacement strip FEAPpv results")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="feappv-compare-") as directory:
        if args.shell_series:
            reference = args.feappv.resolve()
            path = Path(directory)
            cylinder = run_reference(reference, path, "cylinder", shell_deck(), 2)
            strip = run_reference(reference, path, "strip", linear_strip_deck(), 17)
            for step, (axisym, linear) in enumerate(zip(cylinder, strip), 1):
                print("FEAP_SHELL", step, axisym[0])
                print("FEAP_CURL_SMALL", step, linear[2], linear[4])
        else:
            run_comparison(args.feappv.resolve(), args.cpp.resolve(), Path(directory))


if __name__ == "__main__":
    main()
