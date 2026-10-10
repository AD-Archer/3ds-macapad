# Macapad for 3DS tasks. `just` lists them. Builds run in devkitPro's Docker image (Docker Desktop must be running).

emulator := justfile_directory() / "../.tools/azahar-dl/azahar-macos-arm64-2126.2/Azahar.app"

default:
	@just --list

# Build macapad.3dsx (always clean: the Docker VM's clock drifts)
build:
	./build.sh

# Build and open it in the Azahar emulator
run: build
	-pkill -f "Azahar.app/Contents/MacOS/azahar"
	open -n "{{emulator}}" --args "{{justfile_directory()}}/macapad.3dsx"

# Build and send it over Wi-Fi: open the Homebrew Launcher, press Y, then `just send 10.0.0.42`
send ip: build
	docker run --rm -v "{{justfile_directory()}}":/src -w /src devkitpro/devkitarm 3dslink -a {{ip}} macapad.3dsx

# Build and copy it to a mounted SD card, e.g. `just sd /Volumes/NINTENDO`
sd volume: build
	mkdir -p "{{volume}}/3ds"
	cp macapad.3dsx "{{volume}}/3ds/"
	diskutil eject "{{volume}}"

# Build macapad.cia, to install with FBI (packing tools are x86_64 Linux, so this runs under emulation)
cia: build
	docker run --rm --platform linux/amd64 -v "{{justfile_directory()}}":/src -w /src devkitpro/devkitarm ./cia/build-cia.sh

# Remove build output
clean:
	./build.sh clean
