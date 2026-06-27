# Build the codetopo design document PDF.
#
#   make       - pdf
#   make pdf   - typeset codetopo.tex with tectonic
#   make clean - remove LaTeX build artifacts

.PHONY: all pdf clean

all: pdf

pdf:
	tectonic codetopo.tex

clean:
	rm -f *.aux *.bbl *.blg *.out *.toc *.log *.xdv
