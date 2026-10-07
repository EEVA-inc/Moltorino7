from conan import ConanFile
from conan.tools.files import copy
from conan.tools.cmake import CMakeToolchain
from os import path


class Chatterino(ConanFile):
    name = "Chatterino"
    settings = "os", "compiler", "build_type", "arch"
    default_options = {
        "with_benchmark": False,
        "with_openssl3": True,
        "openssl*:shared": True,
        "boost*:header_only": True,
        "hunspell*:shared": False,
        "libwebm/*:with_new_parser_api": True,
        "libwebm/*:with_pes_ts": False,
        "ffmpeg/*:shared": False,
        "ffmpeg/*:avdevice": False,
        "ffmpeg/*:swresample": False,
        "ffmpeg/*:postproc": False,
        "ffmpeg/*:disable_everything": True,
        "ffmpeg/*:enable_decoders": "h264,hevc,vp8,vp9,libdav1d,mpeg4,mjpeg,png,gif,webp",
        "ffmpeg/*:enable_encoders": "libvpx_vp8",
        "ffmpeg/*:enable_demuxers": "mov,matroska,avi,gif,image2,webp_anim",
        "ffmpeg/*:enable_muxers": "webm",
        "ffmpeg/*:enable_parsers": "h264,hevc,vp8,vp9,av1,mpeg4video,mjpeg,png,gif,webp",
        "ffmpeg/*:enable_protocols": "file,pipe",
        "ffmpeg/*:enable_filters": "scale,fps,setpts,setsar,format,transpose,hflip,vflip",
        "ffmpeg/*:with_programs": True,
        "ffmpeg/*:with_bzip2": False,
        "ffmpeg/*:with_lzma": False,
        "ffmpeg/*:with_libiconv": False,
        "ffmpeg/*:with_freetype": False,
        "ffmpeg/*:with_openjpeg": False,
        "ffmpeg/*:with_openh264": False,
        "ffmpeg/*:with_opus": False,
        "ffmpeg/*:with_vorbis": False,
        "ffmpeg/*:with_libx264": False,
        "ffmpeg/*:with_libx265": False,
        "ffmpeg/*:with_libmp3lame": False,
        "ffmpeg/*:with_libfdk_aac": False,
        "ffmpeg/*:with_libwebp": False,
        "ffmpeg/*:with_ssl": False,
        "ffmpeg/*:with_libalsa": False,
        "ffmpeg/*:with_pulse": False,
        "ffmpeg/*:with_vaapi": False,
        "ffmpeg/*:with_vdpau": False,
        "ffmpeg/*:with_xcb": False,
        "ffmpeg/*:with_xlib": False,
        "ffmpeg/*:with_appkit": False,
        "ffmpeg/*:with_avfoundation": False,
        "ffmpeg/*:with_coreimage": False,
        "ffmpeg/*:with_audiotoolbox": False,
        "ffmpeg/*:with_videotoolbox": False,
        "ffmpeg/*:with_libsvtav1": False,
        "ffmpeg/*:with_libaom": False,
        "ffmpeg/*:with_libdav1d": True,
    }
    options = {
        "with_benchmark": [True, False],
        # Qt is built with OpenSSL 3 from version 6.5.0 onwards
        "with_openssl3": [True, False],
    }
    generators = "CMakeDeps"

    def requirements(self):
        self.requires("boost/1.90.0")

        # if self.settings.os != "Windows":
        #     return

        self.requires("libavif/1.4.1")
        self.requires("libwebp/1.6.0")
        self.requires("libvpx/1.16.0", force=True)
        self.requires("libwebm/1.0.0.32")
        self.requires("libyuv/1892")
        self.requires("ffmpeg/7.1.5")
        if self.options.get_safe("with_benchmark", False):
            self.requires("benchmark/1.9.0")

        self.requires("openssl/3.6.1")
        self.requires("hunspell/1.7.2")
        self.requires("keychain/1.3.0")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.blocks.remove("compilers")
        tc.blocks.remove("cmake_flags_init")
        tc.blocks.remove("cppstd")
        tc.blocks.remove("libcxx")
        tc.blocks.remove("generic_system")
        tc.blocks.remove("user_toolchain")
        tc.blocks.remove("output_dirs")
        tc.blocks.remove("apple_system")
        tc.user_presets_path = False
        tc.generate()

        media = self.dependencies["ffmpeg"]
        for destination in ("bin/support/media", "Chatterino2/support/media"):
            media_directory = path.join(self.build_folder, destination)
            for program in ("ffmpeg", "ffprobe"):
                filename = program + (".exe" if self.settings.os == "Windows" else "")
                copy(
                    self,
                    filename,
                    media.cpp_info.bindirs[0],
                    media_directory,
                    keep_path=False,
                )

            for package in ("ffmpeg", "libvpx", "libwebm", "libyuv", "dav1d", "zlib"):
                dependency = self.dependencies[package]
                copy(
                    self,
                    "*",
                    path.join(dependency.package_folder, "licenses"),
                    path.join(media_directory, "licenses", package),
                )

            copy(
                self,
                "ffmpeg-*",
                path.join(self.recipe_folder, "resources", "licenses"),
                path.join(media_directory, "licenses", "ffmpeg"),
            )

        def copy_bin(dep, selector, subdir):
            src = path.realpath(dep.cpp_info.bindirs[0])
            dst = path.realpath(path.join(self.build_folder, subdir))

            if src == dst:
                return

            copy(self, selector, src, dst, keep_path=False)

        for dep in self.dependencies.values():
            # macOS
            copy_bin(dep, "*.dylib", "bin")
            # Windows
            copy_bin(dep, "*.dll", "bin")
            copy_bin(dep, "*.dll", "Chatterino2")  # used in CI
            # Linux
            copy(
                self,
                "*.so*",
                dep.cpp_info.libdirs[0],
                path.join(self.build_folder, "bin"),
                keep_path=False,
            )
