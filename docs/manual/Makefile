# docs/manual/Makefile — build the LaTeX user manuals into PDF.
#
# Requires TeX Live with lualatex + luatexja (ltjsarticle) and the Menlo
# font (a macOS system font; substitute any monospaced font with box-drawing
# glyphs on other platforms).
#
#   make            # build both PDFs (EN + JA)
#   make en / ja    # one language
#   make clean      # remove aux files (keeps PDFs)
#   make distclean  # also remove the PDFs

LATEX = lualatex -interaction=nonstopmode -halt-on-error
AUX   = *.aux *.log *.toc *.out

.PHONY: all en ja clean distclean
all: en ja
en: USERS_MANUAL_EN.pdf
ja: USERS_MANUAL_JA.pdf

USERS_MANUAL_%.pdf: USERS_MANUAL_%.tex
	$(LATEX) $<
	$(LATEX) $<
	@rm -f $(AUX)

clean:
	rm -f $(AUX)
distclean: clean
	rm -f USERS_MANUAL_EN.pdf USERS_MANUAL_JA.pdf
