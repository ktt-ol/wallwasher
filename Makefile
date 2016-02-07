SHELL = bash

index_html.h: index.html Makefile
	@echo -n "" > index_html.h
	@echo -n 'const char INDEX_HTML[] PROGMEM = "' >> index_html.h
	sed 's/"/\\"/g' index.html | tr '\n' ' ' >> index_html.h
	@echo '";' >> index_html.h