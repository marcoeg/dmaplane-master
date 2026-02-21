.PHONY: all clean driver tests

all: driver tests

driver:
	$(MAKE) -C driver

tests:
	$(MAKE) -C tests

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C tests clean
