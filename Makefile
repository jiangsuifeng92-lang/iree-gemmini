.DEFAULT_GOAL := all
SHELL := /bin/bash
.PHONY: all doctor toolchain compiler model firmware dpi vcs run wave audit
all: firmware vcs
doctor:
	source ./env.sh && python3 scripts/doctor.py
toolchain:
	./scripts/build_host_toolchain.sh
compiler:
	./scripts/build_compiler.sh
model: compiler
	./G_arm_gemmini/scripts/compile_model.sh mnist
firmware:
	./scripts/build_firmware.sh
dpi:
	./scripts/build_dpi.sh
vcs:
	./scripts/build_vcs.sh
run: firmware
	./scripts/run_vcs.sh
wave: firmware
	WAVE=1 ./scripts/run_vcs.sh
audit:
	python3 scripts/audit_repo.py
