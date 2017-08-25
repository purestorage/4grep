all:
	@$(MAKE) -C bitmap

install: all
	install -D -m 0755 4grep -t $(DESTDIR)/usr/bin
	install -D -m 0644 bitmap/4grep.so -t $(DESTDIR)/usr/lib

test:
	@$(MAKE) -C bitmap
	@./bitmap/exec/test
	@python ./test.py

clean:
	@$(MAKE) -C bitmap clean
