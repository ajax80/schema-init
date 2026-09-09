# COPR build entry point. COPR invokes:
#   make -f .copr/Makefile srpm outdir=<resultdir> spec=<specfile>
# Build the SRPM from tracked git content only (git archive HEAD), never the
# working tree — a stray built binary in the checkout poisons find-debuginfo.
outdir ?= $(CURDIR)
spec ?= schema-init.spec

srpm:
	dnf -y install rpm-build git
	V=$$(rpmspec -q --srpm --qf '%{version}\n' $(spec)); \
	mkdir -p "$(outdir)"; \
	git archive --format=tar.gz --prefix=schema-init-$$V/ HEAD \
	    -o /tmp/schema-init-$$V.tar.gz; \
	rpmbuild -bs \
	    --define "_sourcedir /tmp" \
	    --define "_srcrpmdir $(outdir)" \
	    "$(spec)"

.PHONY: srpm
