"""Compiler-free policy/preservation guards, not a native ABI or crash proof.

Original fingerprints are from ghostss2016/AcceleratorLocal master,
05149568304a05a3b90c252e805a2803bb78e933. The native regression imports the
production accelerator_runtime.h; central CI owns compilation and execution.
"""
import ast
import hashlib
import os
from pathlib import Path
import re
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]


def body(text, name):
    match = re.search(r'\b' + re.escape(name) + r'\([^;{]*\)\s*(?:override\s*)?\{', text)
    if not match:
        raise AssertionError('Missing function: ' + name)
    start, depth = match.end() - 1, 0
    for end in range(start, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[start:end + 1]
    raise AssertionError('Unclosed function: ' + name)


class Api18Migration(unittest.TestCase):
    def test_real_api18_owned_typed_hooks(self):
        header = (ROOT / 'accelerator_local.h').read_text()
        source = (ROOT / 'accelerator_local.cpp').read_text()
        combined = header + source
        self.assertIn('#if METAMOD_PLAPI_VERSION < 18', header)
        self.assertIn('#error "AcceleratorLocal requires the real MetaMod API18/KHook headers"', header)
        self.assertNotRegex(combined, r'\b(?:SH_\w+|RETURN_META\w*|META_IFACEPTR)\s*\(')
        self.assertNotRegex(combined, r'(?:#\s*define\s+METAMOD_PLAPI_VERSION|GetApiVersion\s*\()')
        self.assertNotRegex(combined, r'\bclass\s+GameSessionConfiguration_t\s*\{')
        self.assertNotRegex(combined, r'\b(?:PatternScan|FindPattern|SignatureScan)\s*\(')
        self.assertIn('SvarogHooks::Virtual<IServerGameDLL, void, bool, bool, bool>', header)
        self.assertIn('SvarogHooks::Virtual<INetworkServerService, void,', header)
        self.assertIn('const GameSessionConfiguration_t&, ISource2WorldSession*, const char*>', header)
        self.assertIn('accelerator::OwnedHooks<FrameHook, StartupHook> hooks_', header)
        load = body(source, 'Load')
        self.assertIn('(&IServerGameDLL::GameFrame, this, nullptr, &AcceleratorLocal::Api18GameFrame)', load)
        self.assertIn('(&INetworkServerService::StartupServer, this, nullptr, &AcceleratorLocal::Api18StartupServer)', load)
        self.assertLess(load.index('hooks_.Install('), load.index('new (std::nothrow)'))
        self.assertLess(load.index('signals_.Prepare('), load.index('hooks_.Install('))

    def test_original_runs_through_khook_once(self):
        source = (ROOT / 'accelerator_local.cpp').read_text()
        for callback in ('Api18GameFrame', 'Api18StartupServer'):
            with self.subTest(callback=callback):
                callback_body = body(source, callback)
                self.assertIn('CallbackActivity::Scope callback(g_CallbackActivity)', callback_body)
                self.assertIn('return {KHook::Action::Ignore}', callback_body)
                self.assertNotIn('->GameFrame(', callback_body)
                self.assertNotIn('->StartupServer(', callback_body)
                self.assertNotIn('Supersede', callback_body)

    def test_fail_clean_and_unload_order(self):
        source = (ROOT / 'accelerator_local.cpp').read_text()
        load = body(source, 'Load')
        failure = load[load.index('if (!exceptionHandler_ || !signals_.Capture(readSignal))'):]
        self.assertLess(failure.index('hooks_.RollbackInitialization()'), failure.index('delete exceptionHandler_'))
        self.assertLess(failure.index('delete exceptionHandler_'), failure.index('exceptionHandler_ = nullptr'))
        self.assertIn('signals_.Reset()', failure)
        unload = body(source, 'Unload')
        self.assertLess(unload.index('hooks_.Clear('), unload.index('delete exceptionHandler_'))
        self.assertLess(unload.index('return false'), unload.index('delete exceptionHandler_'))
        self.assertIn('exceptionHandler_ = nullptr', unload)
        self.assertIn('server_ = nullptr', unload)
        self.assertIn('networkService_ = nullptr', unload)
        self.assertIn('gameServer ? gameServer->GetMapName() : nullptr', load)
        self.assertNotIn('StartupServer({},', load)

    def test_original_raw_dump_and_metadata_writer_preserved(self):
        source = (ROOT / 'accelerator_local.cpp').read_text()
        original = body(source, 'dumpCallback')
        original = original.replace('accelerator::CallbackActivity::Scope callback(g_CallbackActivity);', '')
        normalized = re.sub(r'//[^\n]*|/\*.*?\*/', '', original, flags=re.S)
        normalized = re.sub(r'\s+', '', normalized)
        self.assertEqual(hashlib.sha256(normalized.encode()).hexdigest(),
                         'c0b7870d918c9baaf1089d7cf800f2e0cfbf03e01044c288c923265497880b60')
        self.assertIn('"%s/addons/accelerator_local/dumps"', source)
        self.assertIn('"Phoenix (˙·٠●Феникс●٠·˙), Asher Baker (asherkin)"', source)
        self.assertIn('return "GPL"', source)
        self.assertIn('GNU General Public License, version 3.0', source)

    def test_pinned_breakpad_output_adapter_uses_production_logic(self):
        source = (ROOT / 'accelerator_local.cpp').read_text()
        adapter = body(source, 'PrintProcessState')
        self.assertIn('accelerator::PrintOriginalProcessState(', adapter)
        self.assertIn('&google_breakpad::PrintProcessState', adapter)
        self.assertIn('#include "common/scoped_ptr.h"', source)
        test = (ROOT / 'tests/accelerator_runtime_test.cpp').read_text()
        self.assertIn('accelerator::PrintOriginalProcessState(state, contents, requestingOnly, &resolver', test)
        self.assertIn('assert(!dumpStackPointers && threadIndex == -1)', test)

    def test_existing_native_abi_and_package_are_not_fabricated(self):
        expected = {
            'CMiniDumpComment.hpp': '19167e182cef528a6d91555d5c2fec49f2fce7529ad0c2dd78b0e23ce7dfaab7',
            'LICENSE': '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986',
        }
        for name, digest in expected.items():
            with self.subTest(file=name):
                self.assertEqual(hashlib.sha256((ROOT / name).read_bytes()).hexdigest(), digest)
        # The original complete package logic is unchanged; license notices
        # are appended, without replacing the original binary or VDF mapping.
        package = (ROOT / 'PackageScript').read_bytes()
        original, marker, _ = package.partition(b'\n# Bundled license notices;')
        self.assertTrue(marker)
        self.assertEqual(hashlib.sha256(original).hexdigest(),
                         '1bfe397f5cdf89c3b46e68b5018f55476a696e996e29ccc4f96e09117328957d')
        script = (ROOT / 'AMBuildScript').read_text()
        self.assertIn("self.plugin_name = 'accelerator_local'", script)
        self.assertIn("self.plugin_alias = 'accelerator_local'", script)
        self.assertIn("os.path.join(context.currentSourcePath, '..', 'SchemaEntity')", script)
        self.assertIn("os.path.join(self.mms_root, 'third_party', 'khook', 'include')", script)
        self.assertIn("'breakpad', 'build', 'src', 'client', 'linux', 'libbreakpad_client.a'", script)

    def test_production_package_preserves_vdf_and_unique_license_destinations(self):
        # Execute PackageScript itself. AMBuild graph insertion is the only
        # substituted boundary; no compiler or synthetic ELF is needed.
        class PackageBoundary:
            sourcePath = str(ROOT)

            def __init__(self, directory):
                self.buildPath = directory
                self.folders = set()
                self.copies = {}

            def SetBuildFolder(self, name):
                self.packageFolder = name

            def AddFolder(self, path):
                self.folders.add(path)
                return SimpleNamespace(path=path)

            def AddCopy(self, source, destination):
                if not isinstance(destination, str):
                    destination = os.path.join(destination.path, Path(source).name)
                if destination in self.copies:
                    raise AssertionError('duplicate package output: ' + destination)
                self.copies[destination] = str(source)

        with tempfile.TemporaryDirectory(prefix='accelerator-package-fixture-') as temporary:
            boundary = PackageBoundary(temporary)
            target = SimpleNamespace(arch='x86_64')
            plugin = SimpleNamespace(plugin_name='accelerator_local', plugin_alias='accelerator_local',
                                     all_targets=[SimpleNamespace(target=target)],
                                     binaries=[SimpleNamespace(target=target,
                                                               binary=os.path.join(temporary, 'accelerator_local.so'),
                                                               debug=None)])
            exec(compile((ROOT / 'PackageScript').read_text(), 'PackageScript', 'exec'),
                 {'builder': boundary, 'MMSPlugin': plugin})
            self.assertEqual(boundary.packageFolder, 'package')
            self.assertEqual(set(boundary.copies), {
                'addons/accelerator_local/accelerator_local.so',
                'addons/metamod/accelerator_local.vdf',
                'addons/accelerator_local/licenses/AcceleratorLocal-LICENSE',
                'addons/accelerator_local/licenses/Breakpad-LICENSE',
                'addons/accelerator_local/licenses/LSS-LICENSE',
                'addons/accelerator_local/licenses/libdisasm-LICENSE',
                'addons/accelerator_local/licenses/libdisasm-README',
            })
            vdf = Path(boundary.copies['addons/metamod/accelerator_local.vdf']).read_text()
            self.assertEqual(dict(re.findall(r'"(alias|file)"\s+"([^"]+)"', vdf)), {
                'alias': 'accelerator_local', 'file': 'addons/accelerator_local/accelerator_local',
            })
            expected_sources = {
                'AcceleratorLocal-LICENSE': ROOT / 'LICENSE',
                'Breakpad-LICENSE': ROOT / 'breakpad/src/LICENSE',
                'LSS-LICENSE': ROOT / 'breakpad/src/src/third_party/lss/LICENSE',
                'libdisasm-LICENSE': ROOT / 'breakpad/src/src/third_party/libdisasm/LICENSE',
                'libdisasm-README': ROOT / 'breakpad/src/src/third_party/libdisasm/README.breakpad',
            }
            for name, source in expected_sources.items():
                self.assertEqual(boundary.copies['addons/accelerator_local/licenses/' + name], str(source))

    def test_build_recipes_parse_without_building(self):
        for name in ('configure.py', 'AMBuildScript', 'AMBuilder', 'PackageScript'):
            with self.subTest(file=name):
                ast.parse((ROOT / name).read_text(), filename=name)
        self.assertFalse((ROOT / 'Makefile').exists())
        for workflow in (ROOT / '.github/workflows').glob('*'):
            if workflow.suffix in ('.yml', '.yaml'):
                self.assertRegex(workflow.read_text(), r'(?m)^enabled:\s*false\s*$')

    def test_existing_recipe_handles_sdk_without_tier1_archive(self):
        # Execute the production recipe's standalone resolver, not AMBuild or
        # a copied implementation. The filesystem is the substituted boundary.
        script = (ROOT / 'AMBuildScript').read_text()
        tree = ast.parse(script, filename='AMBuildScript')
        resolver = next(node for node in tree.body
                        if isinstance(node, ast.FunctionDef) and node.name == 'ResolveTier1Library')
        scope = {'os': os}
        module = ast.Module(body=[resolver], type_ignores=[])
        exec(compile(module, 'AMBuildScript:ResolveTier1Library', 'exec'), scope)
        resolve = scope['ResolveTier1Library']
        with tempfile.TemporaryDirectory(prefix='accelerator-sdk-fixture-') as temporary:
            folder = Path(temporary)
            self.assertIsNone(resolve('cs2', str(folder), 'x86_64'))
            with self.assertRaisesRegex(Exception, 'Required SDK library is missing'):
                resolve('csgo', str(folder), 'x86_64')
            (folder / 'tier1.a').touch()
            self.assertEqual(resolve('cs2', str(folder), 'x86_64'), str(folder / 'tier1.a'))
            self.assertEqual(resolve('csgo', str(folder), 'x86_64'), str(folder / 'tier1.a'))
            self.assertEqual(resolve('sdk2013', str(folder), 'x86'), str(folder / 'tier1.a'))
            with self.assertRaisesRegex(Exception, 'tier1_i486.a'):
                resolve('csgo', str(folder), 'x86')
            (folder / 'tier1_i486.a').touch()
            self.assertEqual(resolve('csgo', str(folder), 'x86'), str(folder / 'tier1_i486.a'))
        self.assertIn('tier1 = ResolveTier1Library(sdk.name, lib_folder, compiler.target.arch)', script)
        self.assertIn('if tier1 is not None:\n        compiler.postlink += [tier1]', script)
        self.assertIn("compiler.postlink += [os.path.join(lib_folder, 'interfaces.a')]", script)

    def test_retained_breakpad_sources_use_real_upstream_config(self):
        # Execute the actual recipe helper without compiling. A configured
        # filesystem and compiler graph are the substituted I/O boundary.
        script = (ROOT / 'AMBuildScript').read_text()
        tree = ast.parse(script, filename='AMBuildScript')
        helper = next(node for node in tree.body
                      if isinstance(node, ast.FunctionDef) and node.name == 'ConfigureBreakpadHeaders')
        scope = {'os': os}
        exec(compile(ast.Module(body=[helper], type_ignores=[]),
                     'AMBuildScript:ConfigureBreakpadHeaders', 'exec'), scope)
        configure = scope['ConfigureBreakpadHeaders']
        with tempfile.TemporaryDirectory(prefix='accelerator-breakpad-config-fixture-') as temporary:
            compiler = SimpleNamespace(cxxincludes=['sdk/public'], defines=['EXISTING'])
            with self.assertRaisesRegex(Exception, 'Missing upstream-configured Breakpad header'):
                configure(compiler, temporary)
            self.assertEqual(compiler.cxxincludes, ['sdk/public'])
            self.assertEqual(compiler.defines, ['EXISTING'])
            config_dir = Path(temporary) / 'breakpad/build/src'
            config_dir.mkdir(parents=True)
            (config_dir / 'config.h').touch()
            configure(compiler, temporary)
            self.assertEqual(compiler.cxxincludes, [str(config_dir), 'sdk/public',
                                                   str(Path(temporary) / 'breakpad/src/src')])
            self.assertEqual(compiler.defines, ['EXISTING', 'HAVE_CONFIG_H'])
        self.assertIn('ConfigureBreakpadHeaders(compiler, context.currentSourcePath)', script)
        self.assertNotRegex(script, r'\b(?:HAVE_A_OUT_H|N_UNDF)\s*=')

    def test_native_harness_imports_production_logic(self):
        test = (ROOT / 'tests/accelerator_runtime_test.cpp').read_text()
        self.assertIn('#include "../accelerator_runtime.h"', test)
        self.assertIn('accelerator::OwnedHooks<BoundaryHook, BoundaryHook>', test)
        self.assertIn('accelerator::SignalMonitor monitor', test)
        self.assertIn('accelerator::CrashMetadata metadata', test)
        self.assertIn('accelerator_runtime_test: all checks passed', test)


if __name__ == '__main__':
    unittest.main()
