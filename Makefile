.PHONY: all clean driver tests examples

all: driver tests examples

driver:
	$(MAKE) -C driver

tests:
	$(MAKE) -C tests

examples:
	$(MAKE) -C examples/misc

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C tests clean
	$(MAKE) -C examples/misc clean
