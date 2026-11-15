TECTONIC ?= tectonic
MAIN := main.tex
PDF := main.pdf

.PHONY: all chapters clean

all: $(PDF)

chapters:
	sh tools/convert.sh

$(PDF): $(MAIN) $(wildcard chapters/*.tex)
	$(TECTONIC) -X compile --keep-logs --keep-intermediates $(MAIN)

clean:
	rm -f main.aux main.log main.out main.pdf main.toc main.xdv
	rm -f chapters/*.aux
