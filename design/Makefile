# Build the codetopo design document PDF.
#
#   make       - pdf
#   make pdf   - typeset codetopo.tex with tectonic
#   make clean - remove LaTeX build artifacts

.PHONY: all pdf clean

all: pdf

pdf:
	tectonic -Z shell-escape -Z shell-escape-cwd=. codetopo.tex

clean:
	rm -f *.aux *.bbl *.blg *.out *.toc *.log *.xdv triton.sty
	rm -rf *.triton-cache *.triton-src.triton
