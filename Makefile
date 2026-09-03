# Thin forwarder so `make gif EFFECT=plasma` works from the repo root.
# The real rules live in harness/Makefile.
.PHONY: all gif all-gifs bench clean list
all gif all-gifs bench clean list:
	@$(MAKE) --no-print-directory -f harness/Makefile $@
