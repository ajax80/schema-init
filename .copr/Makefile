# COPR build entry point. COPR invokes:
#   make -f .copr/Makefile srpm outdir=<resultdir> spec=<specfile>
# Build the SRPM from tracked git content only (git archive HEAD), never the
# working tree — a stray built binary in the checkout poisons find-debuginfo.
outdir ?= $(CURDIR)

srpm:
	dnf -y install rpm-build git
	$(MAKE) -f Makefile srpm RELDIR=$(outdir)

.PHONY: srpm
