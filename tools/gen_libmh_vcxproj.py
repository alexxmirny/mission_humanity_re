#!/usr/bin/env python3
"""gen_libmh_vcxproj.py -- generate ALL THREE libmh projects from the one module tree.

TWO COMPILATIONS OF ONE ROSTER, TWO QUESTIONS (the distinction is F4D-PRE's, and getting it wrong
measures the wrong edge -- see tools/check_libmh_outbound.py's header) -- and, since fork F4G,
THREE ARTIFACTS, because the standalone arm ships as a DLL as well as an archive:

  libmh/libmh.vcxproj       the STANDALONE arm: a static lib built WITH MH_LIBMH_BUILD, for a host
                            that has no game image (LIB-REF). Every promotion seam and hook row
                            compiles out by construction, and MH_CRT() picks the VENDORED CRT.
  libmh_std/libmh_std.vcxproj  fork F4G: the standalone arm AS A DLL -- `Release\\standalone\\libmh.dll`,
                            the file configuration (3) actually runs. It COMPILES NO ROSTER TU: it
                            links the archive above with /WHOLEARCHIVE, so the two artifacts are the
                            same object code by construction and the DLL costs one link, not a
                            second 627-TU compile.
  libmh_dll/libmh_dll.vcxproj  the HOSTED arm (fork F4D): libmh.dll, the file that ships beside
                            mh.dll. Built WITHOUT MH_LIBMH_BUILD -- it is the same 627 TUs mh.dll
                            used to compile, so it is the build whose edges decide the split.

WHY THE STANDALONE DLL IS A THIRD ARTIFACT AND NOT libmh_dll's OUTPUT (fork F4G, measured). The
hosted DLL cannot replay standalone and the reason is one macro: `MH_LIBMH_BUILD` is what
crt/crt_select.h reads to choose between the VENDORED CRT (`mh::crt::*`) and the ORIGINAL BINARY's
own Watcom CRT reached at a fixed VA (`mh::call::*`). libmh_dll does not define it, so every
MH_CRT() site in it -- the allocator included -- is a call to an address that exists only when
mh.exe is mapped. A standalone host would fault on the first one. So configuration (2)'s libmh.dll
and configuration (3)'s libmh.dll are necessarily DIFFERENT BINARIES with the same file name, living
in different deployments; a wrong deployment is caught loudly and immediately by mh.dll's own bind
(`LOADED BUT REFUSED -- 0 of N contract symbols`, the f4d_wrong shape).

All three set MH_SPINE_IN_IMAGE, which is what says "this image CONTAINS the spine" and therefore
owns the process-wide singletons (mh::state::live(), owner_table(), owner_count()). mh.vcxproj must
NOT set it; mh_nettest.vcxproj must, because it compiles the roster too. libref_host.vcxproj must
NOT set it SINCE FORK F4G, and that reversal is load-bearing rather than tidy-up: the host now
imports its spine from libmh_std's DLL instead of linking the archive into itself, so a magic static
there would give the HOST its own region registry while the DLL rebased its own (G179, one image
over). The three singletons became import rows of libmh_std.def in the same change, which is the
mechanism working -- dropping the define surfaced them as undefined externals rather than leaving
two silently-diverging copies.

LIB0's packaging half (the endgame plan, tracker LIB0). The static lib assembles every .cpp
under the migrated modules (sim, ai, orders, tact, save, state, lockstep). NOTHING ELSE since
fork F4D-PRE:
mh/addr/mh_calls.gen.cpp -- the VA thunk layer that LIB-ABI/SB-BIND progressively replace -- was
the one named shim and left the roster under ruling Q5 (see NAMED_SHIMS below).
The injection layer (seams/, hook/, effects/, shadow/) is deliberately absent: a consumer that
links libmh.lib and still needs those symbols is reaching the harness, which is exactly the
dependency LIB0's build-time check exists to surface (unresolved externals name the offender).

WHERE THE ROSTER LIVES (fork F5O, 2026-09-16). The 628 roster TUs are no longer under
`src/mh_dll/mh/<domain>/`; they were moved wholesale to `src/mh_dll/libmh/<domain>/` -- the
directory of the project that owns them -- keeping the seven domain subdirectories. So
libmh.vcxproj names them project-relative (`sim\\x.cpp`), and the other two arms reach one
directory over (`..\\libmh\\sim\\x.cpp`), as does mh_nettest.vcxproj.

EXACTLY TWO LOCATIONS ARE EXPLICITLY SHARED between build targets, and both are stated rather
than incidental:
  1. `src/mh_dll/libmh/<domain>/` -- the roster. libmh (archive), libmh_dll (hosted arm) and
     mh_nettest all compile these same TUs; a lib's second CRT arm and its tests compiling the
     lib's own sources from the lib's own directory is the natural reading of "one dir per
     target", not a violation of it.
  2. `src/mh_dll/mh/addr/mh_calls.gen.cpp` -- ruling Q5's PER-IMAGE shim. It is not in the
     archive (see NAMED_SHIMS below) and it is not one project's file either: SIX projects
     compile their own copy (libmh_dll, libmh_std, libref_host, mh, mh_harness, mh_nettest),
     because whichever image compiles the roster must supply the `mh::call::detail::s_*` shapes
     the roster's bodies reference. It stays under mh/addr/ with the rest of the generated VA
     layer. tools/check_libmh_outbound.py rules the resulting unresolved externals as the `shim`
     bucket, so the arrangement is gated rather than merely asserted here.
Everything else under `src/mh_dll/mh/` is mh.dll's own code (plus header-only trees: addr/, crt/,
fp/, config/, fix/, patch/, include/ and the seams/ remainder), which is why `..\\mh` stays on
every include path next to the new `..\\libmh`.

GENERATED, so a new module file joins the lib by rerunning this -- the same
generate-then-drift-gate pattern as the addr/ headers. `--check` fails if the committed vcxproj
is stale (a .cpp was added/removed without regenerating).

FLAG POLICY (deliberate divergence from mh.vcxproj): /arch:IA32 + /fp:precise are applied to the
WHOLE project, not per-file. mh.vcxproj flags 323/356 sim + 77/77 tact files individually
(FP-FLAGS is closing its ai/sim gaps); for the lib there is no reason to except any TU -- the
flags are semantically inert on integer-only code and mandatory on x87-sensitive code, and a
blanket setting cannot rot the way a hand-list does. No /GL (WholeProgramOptimization): the
archive must stay consumable by a non-LTCG standalone host (LIB-REF).

THE .filters TRAVEL WITH THE PROJECT (fork F5P). Each of the three also gets its
`<project>.vcxproj.filters` emitted here, in the same pass and under the same `--check`, because a
filters file is a second copy of the project's item list and a second copy that nothing gates is the
hand-list failure this generator exists to prevent. The RULE is not defined here: the renderer is
tools/gen_vcxproj_filters.py (folder = the item's directory part with leading `..\\` folded away, so
libmh_dll shows `libmh\\sim` and `mh\\addr` as top-level folders), shared verbatim with the nine hand
projects so there is one rule in the tree rather than two that agree by coincidence.
"""

import argparse
import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_vcxproj_filters as vcxfilters  # noqa: E402  (needs the path insert above)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MH_SRC = os.path.join(REPO, "src", "mh_dll", "libmh")
OUT_PATH = os.path.join(REPO, "src", "mh_dll", "libmh", "libmh.vcxproj")
DLL_OUT_PATH = os.path.join(REPO, "src", "mh_dll", "libmh_dll", "libmh_dll.vcxproj")

STD_OUT_PATH = os.path.join(REPO, "src", "mh_dll", "libmh_std", "libmh_std.vcxproj")

MODULES = ("sim", "ai", "orders", "tact", "save", "state", "lockstep")
# EMPTY SINCE FORK F4D-PRE (ruling Q5). It held exactly one entry, `addr\mh_calls.gen.cpp` -- the
# 2625 generated naked marshalling thunks that call INTO the original binary at fixed VAs. It is
# mh.dll machinery, not spine, and carrying it in the ARCHIVE made 19 of its `mh::call::detail::s_*`
# shapes read as part of libmh's export contract (the measured 133 that Q5 takes to 114).
#
# THE THUNKS ARE NOT GONE, THEY ARE PER-IMAGE. Whichever image compiles the roster also compiles the
# shim: mh.vcxproj and mh_nettest.vcxproj already did, and libref_host.vcxproj gained the row at the
# same commit (its /WHOLEARCHIVE would otherwise leave the roster's own `s_*` references unresolved).
# That is the F4B precedent for mh_net_proto's session_info.cpp, applied one layer down: a pure
# static-lib object both images link the copy they use. tools/check_libmh_outbound.py rules the
# resulting unresolved externals as the `shim` bucket and enumerates them, so the relocation is a
# stated, gated fact rather than a hole in the count.
NAMED_SHIMS = ()
GUID = "{7B3E6D1A-2C4F-4A98-9D5E-11B0C8F3A2E7}"

HEADER = """﻿<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="15.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <!-- GENERATED by tools/gen_libmh_vcxproj.py - do not hand-edit; rerun the generator. -->
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Release|Win32">
      <Configuration>Release</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <ItemGroup>
"""

FOOTER = """  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion>
    <ProjectGuid>%(guid)s</ProjectGuid>
    <RootNamespace>libmh</RootNamespace>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
    <ProjectName>libmh</ProjectName>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'" Label="Configuration">
    <ConfigurationType>StaticLibrary</ConfigurationType>
    <UseDebugLibraries>false</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <!-- OutDir IS PINNED; IntDir IS DELIBERATELY NOT (LIB-DISPATCH-SA, 2026-09-11).

       OutDir: built through mh.sln the default lands the .lib in the SOLUTION's Release folder,
       built alone in the PROJECT's. tools/scan_libmh_vas.py reads ONE path, and its drift gate
       SKIPS when the lib is absent, so the solution route would have left the byte ratchet
       silently measuring a stale artifact. Pinning it makes both routes agree.

       IntDir is left at the default, but NOT because the adjudication keys depend on it any
       more. They used to: tools/data/libmh_va_adjudication.json keys every excused occurrence by
       (va, OBJECT PATH, byte signature), the path being the ARCHIVE MEMBER NAME, i.e. the literal
       string handed to cl, so it encoded IntDir's SPELLING and any pin at all invalidated all
       125 rows at once. tools/scan_libmh_vas.py:_obj_key has normalised both sides to the bare
       `<tu>.obj` basename since 2026-09-11, so the keys are route and IntDir independent now and
       this is a style preference, not a load bearing constraint. (F5O moved the roster from mh/
       to libmh/ and the adjudication file needed no re record, which is that fix being cashed.)
       (An XML comment may not contain a double hyphen, which is why this one has none.) -->
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'">
    <OutDir>$(ProjectDir)$(Configuration)\\</OutDir>
  </PropertyGroup>
  <ImportGroup Label="ExtensionSettings">
  </ImportGroup>
  <ImportGroup Label="PropertySheets" Condition="'$(Configuration)|$(Platform)'=='Release|Win32'">
    <Import Project="$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props" Condition="exists('$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props')" Label="LocalAppDataPlatform" />
  </ImportGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'">
    <ClCompile>
      <PrecompiledHeader>NotUsing</PrecompiledHeader>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
      <WarningLevel>Level3</WarningLevel>
      <Optimization>MaxSpeed</Optimization>
      <SDLCheck>true</SDLCheck>
      <PreprocessorDefinitions>WIN32;NDEBUG;_WINDOWS;_CRT_SECURE_NO_WARNINGS;MH_LIBMH_BUILD;MH_SPINE_IN_IMAGE;%%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <ConformanceMode>true</ConformanceMode>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <AdditionalIncludeDirectories>..\\libmh;..\\mh;..\\mh\\include;..\\mh_common;..\\mh_common\\include;..\\..\\mh_net_proto\\include</AdditionalIncludeDirectories>
      <IntrinsicFunctions>true</IntrinsicFunctions>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
      <EnableEnhancedInstructionSet>NoExtensions</EnableEnhancedInstructionSet>
      <FloatingPointModel>Precise</FloatingPointModel>
    </ClCompile>
  </ItemDefinitionGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
  <ImportGroup Label="ExtensionTargets">
  </ImportGroup>
</Project>
"""


# ---------------------------------------------------------------------------------------------
# THE HOSTED ARM -- libmh.dll (fork F4D)
# ---------------------------------------------------------------------------------------------
# Three things differ from the archive above and each one is a decision rather than a default:
#
#   1. NO MH_LIBMH_BUILD. This is the build mh.dll used to contain, so every promotion seam and
#      every hook row compiles IN. Reading the archive instead measures zero edges and would have
#      called the split free (F4D-PRE's headline trap, docs/dll-split.md).
#   2. mh/addr/mh_calls.gen.cpp IS in the roster here, and it is not a contradiction of ruling Q5.
#      Q5 took the thunks out of the ARCHIVE, whose consumers have no game image to call into, and
#      made them PER-IMAGE instead -- every image that compiles the roster compiles the shim. This
#      image's bodies reach the original binary through exactly those `mh::call::detail::s_*`
#      shapes, so it needs its copy, the same way mh.vcxproj / mh_nettest.vcxproj / libref_host
#      already carry theirs.
#   3. /arch:IA32 + /fp:precise are BLANKET here, as in the archive. mh.vcxproj flags 538 of its
#      666 files individually; the blanket form is strictly stronger and cannot rot, and the
#      oracle for the resulting codegen change is the A/B/C recorded-session replay (arm A against
#      the recording's own committed trajectory), not an assertion in this comment.
#
# There is NO import library and there must not be (the F4B .def argument, one level over): a
# single libmh.lib on a future link line would put libmh.dll in that module's IMPORT TABLE, and a
# missing file would then kill process load before one instruction of ours ran. Config (1) is
# exactly the configuration where the file is missing.
DLL_GUID = "{2E51C7A4-9F3D-4B6C-8A17-D0E45B92C381}"

DLL_HEADER = """﻿<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="15.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <!-- GENERATED by tools/gen_libmh_vcxproj.py - do not hand-edit; rerun the generator. -->
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|Win32">
      <Configuration>Debug</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|Win32">
      <Configuration>Release</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <ItemGroup>
"""

DLL_FOOTER = """  </ItemGroup>
  <ItemGroup>
    <None Include="libmh.def" />
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion>
    <ProjectGuid>%(guid)s</ProjectGuid>
    <Keyword>Win32Proj</Keyword>
    <RootNamespace>libmh_dll</RootNamespace>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
    <ProjectName>libmh_dll</ProjectName>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration">
    <ConfigurationType>DynamicLibrary</ConfigurationType>
    <UseDebugLibraries>true</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'" Label="Configuration">
    <ConfigurationType>DynamicLibrary</ConfigurationType>
    <UseDebugLibraries>false</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <!-- WholeProgramOptimization OFF, matching libmh.vcxproj and mh_net.vcxproj rather than
         mh.dll. Two reasons and neither is performance: /GL emits ANONYMOUS OBJECTs that dumpbin
         cannot read, and the F4D contract is DERIVED from this project's objects
         (tools/gen_libmh_contract.py); an unreadable object set would leave the export list a
         hand-maintained one, which is the G106 shape the whole generator exists to avoid. -->
    <WholeProgramOptimization>false</WholeProgramOptimization>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <!-- The PROJECT is libmh_dll (a project may not share a name with the libmh static lib beside
       it); the FILE is libmh.dll, which is the name the .def declares and the name mh.dll composes
       beside itself at bind time. Pinned rather than left to default, so the two cannot drift. -->
  <PropertyGroup>
    <TargetName>libmh</TargetName>
    <!-- IntDir PINNED, unlike libmh.vcxproj's (whose default is load-bearing for the VA
         adjudication file's object-path keys, see the long note in that generated project). Two
         tools read THIS directory as the hosted roster's object set, and their default is a
         constant in Python: tools/check_libmh_outbound.py's OBJ arm and
         tools/gen_libmh_contract.py's derivation. A default that moves when a toolset changes its
         mind about $(Platform) subdirectories would take both with it, and the failure mode is a
         permanent exit 2, i.e. a gate that has quietly stopped running. -->
    <IntDir>$(Configuration)\\</IntDir>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">
    <LinkIncremental>true</LinkIncremental>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'">
    <LinkIncremental>false</LinkIncremental>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <PrecompiledHeader>NotUsing</PrecompiledHeader>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
      <WarningLevel>Level3</WarningLevel>
      <ConformanceMode>true</ConformanceMode>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <PreprocessorDefinitions>WIN32;_WINDOWS;_USRDLL;_CRT_SECURE_NO_WARNINGS;MH_SPINE_IN_IMAGE;%%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <AdditionalIncludeDirectories>$(ProjectDir)..\\libmh;$(ProjectDir)..\\mh;$(ProjectDir)..\\mh\\include;$(ProjectDir)..\\mh_common;$(ProjectDir)..\\mh_common\\include;$(ProjectDir)..\\..\\mh_net_proto\\include;%%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <EnableEnhancedInstructionSet>NoExtensions</EnableEnhancedInstructionSet>
      <FloatingPointModel>Precise</FloatingPointModel>
    </ClCompile>
    <Link>
      <SubSystem>Windows</SubSystem>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <ModuleDefinitionFile>libmh.def</ModuleDefinitionFile>
      <!-- mh_common.lib carries the buffer codecs the save modules use (mh::lzw, mh::lzss): the
           `common` bucket check_libmh_outbound.py rules. It is a STATIC library, so only the
           objects this image actually uses come across; mh.dll links its own copy. Nothing of
           OURS is linked beyond it: there is no mh.lib here and there must not be, because an
           import of mh.dll would invert the load order this whole split is arranged around.
           (An XML comment may not contain a double hyphen, which is why this one has none.) -->
      <AdditionalDependencies>$(OutDir)mh_common.lib;kernel32.lib;user32.lib;%%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">
    <ClCompile>
      <Optimization>Disabled</Optimization>
      <!-- NO RuntimeLibrary OVERRIDE, mirroring mh_net.vcxproj and mh.vcxproj: the static CRT is
           pinned in Release only, and Release is the binary that ships, that a lane deploys and
           that check_module_bind.py subset reads. -->
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'">
    <ClCompile>
      <PreprocessorDefinitions>NDEBUG;%%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <Optimization>MaxSpeed</Optimization>
      <FunctionLevelLinking>true</FunctionLevelLinking>
      <IntrinsicFunctions>true</IntrinsicFunctions>
      <!-- STATIC CRT, and here it is the SUBSET RULE rather than a preference. libmh.dll is loaded
           from inside mh.dll's DLL_PROCESS_ATTACH; a dynamic CRT would put VCRUNTIME140 and
           MSVCP140 in this module's import table, which mh.dll does not import, and the loader
           would then initialise two fresh DLLs under a lock we hold. Measured with it pinned:
           this module imports KERNEL32 and USER32 and nothing else, so the rule holds with room
           to spare. It also means TWO static CRTs in the process, i.e. two heaps: nothing may
           cross this boundary owning memory, and the contract rows were reviewed for that.
           (An XML comment may not contain a double hyphen, which is why this one has none.) -->
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
    </ClCompile>
  </ItemDefinitionGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
  <ImportGroup Label="ExtensionTargets">
  </ImportGroup>
</Project>
"""

# Compiled by libmh.dll on top of the roster. `addr\\mh_calls.gen.cpp` is Q5's per-image shim (see
# the note above); `libmh_dllmain.cpp` is the inert DllMain plus the two module-level entries.
DLL_EXTRA = ("..\\mh\\addr\\mh_calls.gen.cpp", "libmh_dllmain.cpp")


# ---------------------------------------------------------------------------------------------
# THE STANDALONE ARM AS A DLL -- Release\standalone\libmh.dll (fork F4G)
# ---------------------------------------------------------------------------------------------
# CONFIGURATION (3) IS "libmh + a host, no game binary" (the fork plan), and until this project
# the only libmh a host could have was an ARCHIVE linked into it -- which makes "a real dynamic
# library, standalone-capable" (the plan's original words for libmh.dll, amended at F5L to name
# both builds) an untested claim. This is the file that makes it testable.
#
# IT COMPILES NO ROSTER TU, and that is the whole design rather than a shortcut:
#
#   * `/WHOLEARCHIVE:libmh.lib` pulls EVERY object of the archive into the image, so the DLL and the
#     archive are the same object code by construction -- there is no second compilation to drift,
#     no second set of flags to keep in step, and the build costs one link instead of 632 compiles.
#   * /WHOLEARCHIVE is also the property libref_host.vcxproj was built around ("every object in the
#     artifact must resolve" -- it is how three declared-never-defined symbols were found). Moving
#     the host onto the DLL would have LOST that if the DLL link did not carry it; it carries it.
#   * `addr\mh_calls.gen.cpp` is here for exactly the reason libref_host.vcxproj already carries it
#     (ruling Q5's per-image shim): the archive still REFERENCES a handful of the marshalling
#     shapes, so /WHOLEARCHIVE leaves them unresolved without a copy in the image. It is not a
#     hosted build -- the thunks take the target VA as an argument and no live path here reaches
#     one, and tools/scan_libmh_vas.py's subject is still the ARCHIVE, which this row does not touch.
#
# THERE IS AN IMPORT LIBRARY HERE, and it is the deliberate INVERSE of libmh_dll's "there is no
# import library and there must not be". That rule protects configuration (1): a static import of
# libmh.dll would kill mh.exe at load when the file is absent, and absence IS a shipped
# configuration there. A standalone host has no configuration (1) to degrade to -- libmh IS the
# program -- so a missing DLL killing the process at load is the correct and loudest answer.
STD_GUID = "{6C1F84D2-3A70-4E58-9B44-7D2E1A5C0F93}"

STD_TEMPLATE = """﻿<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="15.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <!-- GENERATED by tools/gen_libmh_vcxproj.py - do not hand-edit; rerun the generator. -->
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Release|Win32">
      <Configuration>Release</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <ItemGroup>
%(compiles)s  </ItemGroup>
  <ItemGroup>
    <None Include="libmh_std.def" />
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion>
    <ProjectGuid>%(guid)s</ProjectGuid>
    <Keyword>Win32Proj</Keyword>
    <RootNamespace>libmh_std</RootNamespace>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
    <ProjectName>libmh_std</ProjectName>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'" Label="Configuration">
    <ConfigurationType>DynamicLibrary</ConfigurationType>
    <UseDebugLibraries>false</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <WholeProgramOptimization>false</WholeProgramOptimization>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <!-- THE FILE IS libmh.dll AND IT LIVES IN ITS OWN DIRECTORY. Configuration (2)'s libmh.dll (the
       HOSTED arm, libmh_dll.vcxproj) lands in Release\\; this one lands in Release\\standalone\\.
       The two are different binaries with one name because the name is what a deployment calls its
       spine, and the two deployments never share a folder. -->
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'">
    <TargetName>libmh</TargetName>
    <OutDir>$(SolutionDir)Release\\standalone\\</OutDir>
    <IntDir>$(ProjectDir)$(Configuration)\\</IntDir>
    <LinkIncremental>false</LinkIncremental>
  </PropertyGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'">
    <ClCompile>
      <PrecompiledHeader>NotUsing</PrecompiledHeader>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
      <WarningLevel>Level3</WarningLevel>
      <Optimization>MaxSpeed</Optimization>
      <PreprocessorDefinitions>WIN32;NDEBUG;_WINDOWS;_USRDLL;_CRT_SECURE_NO_WARNINGS;MH_LIBMH_BUILD;MH_SPINE_IN_IMAGE;%%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <ConformanceMode>true</ConformanceMode>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <AdditionalIncludeDirectories>$(ProjectDir)..\\libmh;$(ProjectDir)..\\mh;$(ProjectDir)..\\mh\\include;$(ProjectDir)..\\mh_common;$(ProjectDir)..\\mh_common\\include;$(ProjectDir)..\\..\\mh_net_proto\\include;%%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <IntrinsicFunctions>true</IntrinsicFunctions>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
      <EnableEnhancedInstructionSet>NoExtensions</EnableEnhancedInstructionSet>
      <FloatingPointModel>Precise</FloatingPointModel>
    </ClCompile>
    <Link>
      <SubSystem>Windows</SubSystem>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <GenerateMapFile>true</GenerateMapFile>
      <ModuleDefinitionFile>libmh_std.def</ModuleDefinitionFile>
      <!-- /LTCG is required because mh_common is built with /GL, exactly as libref_host.vcxproj
           has always needed it. -->
      <LinkTimeCodeGeneration>UseLinkTimeCodeGeneration</LinkTimeCodeGeneration>
      <AdditionalDependencies>$(ProjectDir)..\\libmh\\Release\\libmh.lib;$(SolutionDir)Release\\mh_common.lib;$(SolutionDir)Release\\mh_net_proto.lib;kernel32.lib;user32.lib;ws2_32.lib;advapi32.lib;%%(AdditionalDependencies)</AdditionalDependencies>
      <AdditionalOptions>/WHOLEARCHIVE:$(ProjectDir)..\\libmh\\Release\\libmh.lib %%(AdditionalOptions)</AdditionalOptions>
    </Link>
  </ItemDefinitionGroup>
  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
  <ImportGroup Label="ExtensionTargets">
  </ImportGroup>
</Project>
"""

# The two TUs this image compiles for itself. `addr\\mh_calls.gen.cpp` is Q5's per-image shim (see
# the note above); `libmh_std_dllmain.cpp` is the inert DllMain the satellite rule requires of every
# module in this tree -- inert here for a different reason than the satellites' (nothing loads this
# under a loader lock), which is why it says so in its own file rather than borrowing theirs.
STD_COMPILES = ("..\\mh\\addr\\mh_calls.gen.cpp", "libmh_std_dllmain.cpp")


def render_std():
    compiles = "".join('    <ClCompile Include="%s" />\n' % rel for rel in STD_COMPILES)
    return STD_TEMPLATE % {"guid": STD_GUID, "compiles": compiles}


def sources():
    out = []
    for mod in MODULES:
        base = os.path.join(MH_SRC, mod)
        for root, _dirs, files in os.walk(base):
            for fn in sorted(files):
                if fn.endswith(".cpp"):
                    rel = os.path.relpath(os.path.join(root, fn), MH_SRC)
                    out.append(rel.replace("/", "\\"))
    out.sort()
    return list(NAMED_SHIMS) + out


def render():
    lines = [HEADER]
    for rel in sources():
        lines.append('    <ClCompile Include="%s" />\n' % rel)
    lines.append(FOOTER % {"guid": GUID})
    return "".join(lines)


def render_dll():
    lines = [DLL_HEADER]
    for rel in sources():
        lines.append('    <ClCompile Include="..\\libmh\\%s" />\n' % rel)
    for rel in DLL_EXTRA:
        lines.append('    <ClCompile Include="%s" />\n' % rel)
    lines.append(DLL_FOOTER % {"guid": DLL_GUID})
    return "".join(lines)


OUTPUTS = (
    (OUT_PATH, render, "libmh.vcxproj"),
    (DLL_OUT_PATH, render_dll, "libmh_dll.vcxproj"),
    (STD_OUT_PATH, render_std, "libmh_std.vcxproj"),
)


def render_all(root=None):
    """[(path, text, label)] -- the three projects AND their three .filters, in that pairing.

    `root`, when given, relocates every path into a temp tree; the TEXT is unchanged, which is what
    lets selftest() plant a defect on disk and watch the comparison below go red."""
    out = []
    for path, render_fn, label in OUTPUTS:
        text = render_fn()
        fpath = vcxfilters.filters_path_for(path)
        ftext = vcxfilters.render_from_vcxproj_text(vcxfilters.project_name(path), text)
        if root is not None:
            path = os.path.join(root, os.path.basename(path))
            fpath = os.path.join(root, os.path.basename(fpath))
        out.append((path, text, label))
        out.append((fpath, ftext, label + ".filters"))
    return out


def verify(pairs):
    """--check over already-rendered (path, text, label) triples."""
    rc = 0
    for path, text, label in pairs:
        if not os.path.exists(path):
            print("[gen_libmh_vcxproj] FAIL: %s does not exist -- run the generator" % path)
            rc = 1
            continue
        if io.open(path, encoding="utf-8-sig").read() != text.lstrip("﻿"):
            print(
                "[gen_libmh_vcxproj] FAIL: committed %s is stale -- a module .cpp changed, or the "
                "file was hand-edited; rerun tools/gen_libmh_vcxproj.py" % label
            )
            rc = 1
            continue
        print("[gen_libmh_vcxproj] OK: %s is current" % label)
    return rc


def selftest():
    """Planted staleness must go RED, and the walker must not have quietly emptied.

    The --check row is only worth a gate slot if it can fail. Both halves are fired: a stale
    .vcxproj (the original contract) and a stale .filters (F5P's addition, the one a hand-edit in
    Visual Studio's Solution Explorer would produce -- dragging a file between folders rewrites the
    .filters and nothing else, so that edit is invisible to every other check in this repo)."""
    import shutil
    import tempfile

    ok = True

    def ck(what, cond):
        nonlocal ok
        print("  %-64s %s" % (what, "PASS" if cond else "FAIL"))
        if not cond:
            ok = False

    pairs = render_all()
    ck("six outputs: three projects + three .filters", len(pairs) == 6)
    ck("the roster is not empty (>= 500 TUs)", len(sources()) >= 500)
    lib_f = dict((lbl, txt) for _p, txt, lbl in pairs)["libmh.vcxproj.filters"]
    dll_f = dict((lbl, txt) for _p, txt, lbl in pairs)["libmh_dll.vcxproj.filters"]
    ck("libmh's filters name its own domain folders", '<Filter Include="ai">' in lib_f)
    ck("libmh_dll's filters fold the `..` away", '<Filter Include="libmh\\ai">' in dll_f)
    ck(
        "libmh_dll's filters carry the per-image shim's folder",
        '<Filter Include="mh\\addr">' in dll_f,
    )
    for _p, txt, lbl in pairs:
        if not lbl.endswith(".filters"):
            continue
        vtxt = dict((l2, t2) for _p2, t2, l2 in pairs)[lbl[: -len(".filters")]]
        bad = vcxfilters.validate_pair(vtxt, txt, lbl)
        ck("%s loads (item sets + folder parents agree)" % lbl, bad == [])

    d = tempfile.mkdtemp(prefix="libmh_vcx_selftest_")
    try:
        tmp = render_all(root=d)
        for path, text, _lbl in tmp:
            io.open(path, "w", encoding="utf-8", newline="\r\n").write(text)
        ck("a freshly written tree passes --check", verify(tmp) == 0)

        fpath = [p for p, _t, lbl in tmp if lbl == "libmh_dll.vcxproj.filters"][0]
        good = io.open(fpath, encoding="utf-8-sig").read()
        io.open(fpath, "w", encoding="utf-8", newline="").write(
            good.replace("<Filter>libmh\\sim</Filter>", "<Filter>Source Files</Filter>", 1)
        )
        ck("planted STALE .filters -> --check RED", verify(tmp) == 1)
        io.open(fpath, "w", encoding="utf-8", newline="").write(good)
        ck("restored -> --check green again", verify(tmp) == 0)
        os.remove(fpath)
        ck("planted MISSING .filters -> --check RED", verify(tmp) == 1)

        vpath = [p for p, _t, lbl in tmp if lbl == "libmh.vcxproj"][0]
        vgood = io.open(vpath, encoding="utf-8-sig").read()
        io.open(vpath, "w", encoding="utf-8", newline="").write(
            vgood.replace('<ClCompile Include="ai\\ai_build.cpp" />\n', "", 1)
        )
        ck("planted STALE .vcxproj -> --check RED", verify(tmp) == 1)
    finally:
        shutil.rmtree(d, ignore_errors=True)

    print("gen_libmh_vcxproj --selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--check", action="store_true", help="fail if a committed vcxproj/.filters is stale"
    )
    ap.add_argument("--selftest", action="store_true", help="planted-staleness reds + liveness")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    pairs = render_all()
    if args.check:
        return verify(pairs)
    for path, text, _label in pairs:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        io.open(path, "w", encoding="utf-8", newline="\r\n").write(text)
        n = text.count(" Include=") - text.count("<Filter Include=")
        if not path.endswith(".filters"):
            n -= text.count("<ProjectConfiguration Include=")
        print("wrote %s (%d entries)" % (os.path.relpath(path, REPO), n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
