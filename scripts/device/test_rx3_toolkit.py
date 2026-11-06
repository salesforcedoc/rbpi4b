#!/usr/bin/env python3
"""Host-only toolkit integration tests. Never launches rbp or touches services."""
import contextlib
import importlib.util
import io
import json
import os
import re
import shutil
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('toolkit', HERE / 'rx3-toolkit.py')
toolkit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolkit)


class ToolkitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.chroot = self.root / 'chroot'
        self.bundle = self.root / 'bundle'
        self.baseline = self.chroot / 'root/pdj/rbp'
        self.baseline.parent.mkdir(parents=True)
        self.original = b'AAAABBBB' + bytes(64)
        self.baseline.write_bytes(self.original)
        self.bundle.mkdir()
        (self.bundle / 'core.so').write_bytes(b'fixture library, never executed')
        self.manifest = dict(schema=1, revision=toolkit.REVISION,
            base_sha256=[toolkit.digest(self.original)],
            patches={'beatjump32': [dict(offset=0, before=b'AAAA'.hex(), after=b'JJJJ'.hex(), name='jump')],
                     'keyshift': [dict(offset=4, before=b'BBBB'.hex(), after=b'KKKK'.hex(), name='image')]},
            files=[dict(source='core.so', target='/usr/lib/librx3_core.so', mode=0o755,
                        sha256=toolkit.digest((self.bundle / 'core.so').read_bytes()))])
        self.save()

    def save(self):
        (self.bundle / 'manifest.json').write_text(json.dumps(self.manifest))

    def prepare(self, jump, key):
        with contextlib.redirect_stdout(io.StringIO()):
            return toolkit.prepare(self.bundle, self.chroot, '/root/pdj/rbp', jump, key)

    def test_feature_combinations_and_baseline_preservation(self):
        for jump, key in ((0, 0), (1, 0), (0, 1), (1, 1)):
            target = self.prepare(jump, key)
            self.assertEqual(target.read_bytes()[:8], (b'JJJJ' if jump else b'AAAA') + (b'KKKK' if key else b'BBBB'))
            self.assertEqual(self.baseline.read_bytes(), self.original)
            self.assertEqual(target.stat().st_mode & 0o777, 0o755)
        self.assertEqual(self.prepare(0, 1).read_bytes()[:4], b'AAAA')

    def test_unknown_player_and_guard_fail_closed(self):
        self.baseline.write_bytes(b'foreign player')
        with self.assertRaisesRegex(ValueError, 'unsupported baseline'):
            self.prepare(1, 1)
        self.assertFalse((self.chroot / 'usr/lib/librx3_core.so').exists())
        self.baseline.write_bytes(self.original)
        self.manifest['patches']['keyshift'][0]['before'] = b'NOPE'.hex()
        self.save()
        with self.assertRaisesRegex(ValueError, 'instruction mismatch'):
            self.prepare(1, 1)
        self.assertEqual(self.baseline.read_bytes(), self.original)

    def test_corrupt_bundle_is_validated_before_staging(self):
        (self.bundle / 'core.so').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.prepare(1, 1)
        self.assertFalse((self.chroot / 'root/pdj/rbp.toolkit/rbp').exists())

    def test_revision_overlap_and_path_guards(self):
        self.manifest['revision'] = 'other'
        self.save()
        with self.assertRaisesRegex(ValueError, 'unsupported toolkit'):
            self.prepare(1, 1)
        self.manifest['revision'] = toolkit.REVISION
        self.manifest['patches']['keyshift'] = self.manifest['patches']['beatjump32']
        self.save()
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            self.prepare(1, 1)
        with self.assertRaisesRegex(ValueError, 'escapes'):
            toolkit.within(self.chroot.resolve(), '../outside')

    def launch_block(self, jump, key, extra=None):
        # Execute ONLY the new optional setup block, never the real launcher.
        text = (HERE / 'start-rb.sh').read_text()
        block = text.split('# --- optional toolkit:', 1)[1].split('# --- 5.', 1)[0]
        block = block.split('\n', 1)[1]
        script = block + '\nprintf "RESULT:%s|%s|%s\\nGUARD:%s|%s\\n" "$RB_PLAYER" "$RB_LD_PRELOAD" "${RX3_KEYSHIFT:-0}" "${RX3_PI_PLAYER:-}" "${RX3_PI_LOADER:-}"\n'
        env = dict(os.environ, HERE=str(HERE), RB_TOOLKIT_BEATJUMP32=str(jump),
                   RB_TOOLKIT_KEYSHIFT=str(key), RB_TOOLKIT_BUNDLE=str(self.bundle),
                   RB_CHROOT=str(self.chroot), RB_PLAYER='/root/pdj/rbp',
                   RB_LOADER='/lib/ld-linux.so.3', RB_LD_PRELOAD='existing-shims')
        for name in tuple(env):
            if name.startswith('RB_TOOLKIT_') and name not in ('RB_TOOLKIT_BEATJUMP32', 'RB_TOOLKIT_KEYSHIFT', 'RB_TOOLKIT_BUNDLE'):
                env.pop(name)
        env.update(extra or {})
        for name in ('RX3_KEYSHIFT', 'RX3_PI_PLAYER', 'RX3_PI_LOADER'):
            env.pop(name, None)
        return subprocess.run(['sh', '-c', script], env=env, capture_output=True, text=True)

    def test_launcher_opt_in_and_original_preload_order(self):
        for jump, key in ((0, 0), (1, 0), (0, 1), (1, 1)):
            result = self.launch_block(jump, key)
            self.assertEqual(result.returncode, 0, result.stderr)
            expected = '/root/pdj/rbp.toolkit/rbp' if jump or key else '/root/pdj/rbp'
            preload = 'existing-shims' + (':/usr/lib/librx3_core.so' if key else '')
            self.assertIn('RESULT:%s|%s|%s' % (expected, preload, key), result.stdout)
            guard = expected + '|/lib/ld-linux.so.3' if key else (expected + '|' if jump else '|')
            self.assertIn('GUARD:' + guard + '\n', result.stdout)
            self.assertEqual(self.baseline.read_bytes(), self.original)
        self.assertNotEqual(self.launch_block(2, 0).returncode, 0)

    def test_runtime_ready_marker_must_match_current_pid(self):
        text = (HERE / 'start-rb.sh').read_text()
        block = text.split('if [ "${RB_TOOLKIT_CORE:-0}" = 1 ] &&', 1)[1].split('# --- 10.', 1)[0]
        block = 'if [ "${RB_TOOLKIT_CORE:-0}" = 1 ] &&' + block
        marker = self.chroot / 'tmp/rx3-performance.ready'
        marker.parent.mkdir()
        env = dict(os.environ, RB_TOOLKIT_CORE='1', RB_CHROOT=str(self.chroot), RBP='444')
        script = 'rb_bootmsg() { :; };\n' + block
        for contents, expected in (('', 1), ('other\n', 1), ('444\n', 0)):
            marker.write_text(contents)
            result = subprocess.run(['sh', '-c', script], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, expected)

    def extended_bundle(self):
        self.manifest['schema'] = 2
        self.manifest['modules'] = ['stems', 'stemwave']
        self.manifest['patches']['immediate'] = [dict(offset=8, before='00000000',
                                                     after='49494949', name='immediate')]
        self.save()

    def test_each_extended_feature_and_disable_restores_baseline(self):
        self.extended_bundle()
        for flag in toolkit.FLAGS:
            if flag in ('render_probe', 'logo'):
                continue
            config = toolkit.configuration({'RB_TOOLKIT_' + flag.upper():'1'})
            with contextlib.redirect_stdout(io.StringIO()):
                result = toolkit.prepare(self.bundle, self.chroot, '/root/pdj/rbp', 0, 0, config)
            data = result.read_bytes()
            self.assertEqual(data[4:8], b'KKKK' if config['core'] else b'BBBB', flag)
            self.assertEqual(data[8:12], b'IIII' if flag == 'immediate' else bytes(4), flag)
            if flag == 'key_sync':
                self.assertEqual((config['keyshift'], config['key_match']), (1,1))
            self.assertEqual(self.prepare(0,0).read_bytes(), self.original)

    def test_extended_launcher_dependencies_and_feature_without_keyshift(self):
        self.extended_bundle()
        for feature in ('SEARCH_LATIN', 'SAMPLES', 'KEY_SYNC', 'BROWSE_COLUMNS', 'NOW_PLAYING', 'STEMS'):
            result = self.launch_block(0, 0, {'RB_TOOLKIT_' + feature:'1'})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('existing-shims:/usr/lib/librx3_core.so', result.stdout)
            self.assertIn('GUARD:/root/pdj/rbp.toolkit/rbp|/lib/ld-linux.so.3', result.stdout)
            env_text = (self.chroot / 'tmp/rx3-toolkit.env').read_text()
            self.assertIn('export RX3_KEYSHIFT=' + ('1' if feature == 'KEY_SYNC' else '0'), env_text)
        for mode in ('dark', 'light', 'wait', 'switch'):
            result = self.launch_block(0,0, {'RB_TOOLKIT_THEME':mode})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('existing-shims:/usr/lib/librx3_core.so', result.stdout)

    def test_invalid_extended_options_and_old_bundle_refused(self):
        for name,value in (('SAMPLES','2'), ('THEME','white'), ('KEY_SYNC_RANGE','13'),
                           ('KEY_SYNC_MODE','whatever'), ('BROWSE_FIELD','9'),
                           ('KEY_MATCH_RULES','16'), ('NOW_PLAYING_HOST','host.invalid'),
                           ('SAMPLES_DIR','relative'), ('DECODER_SLEEP_NS','0'), ('STEMS_DIR','relative')):
            with self.assertRaises(ValueError, msg=name):
                toolkit.configuration({'RB_TOOLKIT_' + name:value})
        config = toolkit.configuration({'RB_TOOLKIT_SAMPLES':'1'})
        with self.assertRaisesRegex(ValueError, 'schema 2'):
            toolkit.prepare(self.bundle, self.chroot, '/root/pdj/rbp', 0, 0, config)
        self.assertFalse((self.chroot / 'root/pdj/rbp.toolkit/rbp').exists())

    def test_samples_validation_and_logo_validation_before_publish(self):
        import struct
        import wave
        self.extended_bundle()
        bank = self.chroot / 'root/pdj/rx3-samples'
        bank.mkdir()
        path = bank / '1.wav'
        with wave.open(str(path), 'wb') as output:
            output.setparams((2,2,44100,0,'NONE','not compressed'))
            output.writeframes(bytes(16))
        toolkit.validate_samples(bank)
        (bank / 'settings.ini').write_text('version=1\npad1.mode=3\nvolume_default=50\n')
        config = toolkit.configuration({'RB_TOOLKIT_SAMPLES':'1'})
        with contextlib.redirect_stdout(io.StringIO()):
            toolkit.prepare(self.bundle,self.chroot,'/root/pdj/rbp',0,0,config)
        for data in (b'version=1\nversion=1\n', b'volume_default=50\n',
                     b'version=1\npad1.color=#invalid\n', b'version=1\npad1.mode=4\n'):
            with self.assertRaises(ValueError):
                toolkit.validate_sample_settings(data)
        with wave.open(str(path), 'wb') as output:
            output.setparams((1,2,48000,0,'NONE','not compressed'))
            output.writeframes(bytes(16))
        with self.assertRaisesRegex(ValueError, 'stereo PCM16'):
            toolkit.validate_samples(bank)
        logo = bank / 'logo.rgb565'
        logo.write_bytes(b'RX3LOGO1' + struct.pack('<II',2,1) + bytes(4))
        self.assertEqual(toolkit.validate_logo(logo), (2,1))
        logo.write_bytes(b'RX3LOGO1' + struct.pack('<II',900,1) + bytes(1800))
        with self.assertRaises(ValueError):
            toolkit.validate_logo(logo)

    def test_environment_quotes_data_and_has_light_sample_assets(self):
        config = toolkit.configuration({'RB_TOOLKIT_SAMPLES':'1',
                    'RB_TOOLKIT_SAMPLES_DIR':"/root/pdj/a 'bank' $(touch never)"})
        env = toolkit.launch_environment(config, '/root/pdj/rbp')
        self.assertEqual(env['RX3_SAMPLES_DIR'], config['samples_dir'])
        self.assertEqual(env['RX3_TAB_LIGHT_04'], '/root/pdj/rx3-samples-selected-light.rgb565')
        self.assertEqual(env['RX3_TAB_DARK_01'], '')
        self.assertEqual(env['RX3_TAB_DARK_00'], '')
        self.assertEqual(env['RX3_KEY_SYNC'], '0')

    def test_shell_export_file_treats_sample_path_as_data(self):
        self.extended_bundle()
        marker = self.root / 'must-not-execute'
        directory = "/root/pdj/a 'bank' $(touch " + str(marker) + ')'
        result = self.launch_block(0,0, {'RB_TOOLKIT_SAMPLES':'1', 'RB_TOOLKIT_SAMPLES_DIR':directory})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(marker.exists())
        result = subprocess.check_output(['sh','-c', '. "$1"; printf "%s" "$RX3_SAMPLES_DIR"',
                              'sh', str(self.chroot / 'tmp/rx3-toolkit.env')], text=True)
        self.assertEqual(result, directory)
        self.assertFalse(marker.exists())

    def test_asset_preparer_and_failed_conversion_publishes_nothing(self):
        import struct
        import wave
        script = HERE.parent / 'prepare-rx3-toolkit-assets.py'
        audio = self.root / 'sample.wav'
        with wave.open(str(audio),'wb') as output:
            output.setparams((2,2,44100,0,'NONE','not compressed'))
            output.writeframes(bytes(16))
        logo = self.root / 'logo.rgb565'
        logo.write_bytes(b'RX3LOGO1' + struct.pack('<II',2,1) + bytes(4))
        target = self.root / 'assets'
        result = subprocess.run(['python3',str(script),'--output',str(target),
                                 '--samples',str(audio),'--mode','hold','--logo',str(logo)],
                                 text=True,capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        toolkit.validate_samples(target / 'rx3-samples')
        toolkit.validate_sample_settings((target / 'rx3-samples/settings.ini').read_bytes())
        self.assertIn('pad1.mode=1',(target / 'rx3-samples/settings.ini').read_text())
        self.assertEqual((target / 'rx3-logo-main.rgb565').read_bytes(),logo.read_bytes())
        bad_output = self.root / 'bad-assets'
        invalid = self.root / 'invalid.wav'
        invalid.write_bytes(b'not audio')
        result = subprocess.run(['python3',str(script),'--output',str(bad_output),
                                 '--samples',str(invalid)], text=True,capture_output=True)
        self.assertNotEqual(result.returncode,0)
        self.assertFalse(bad_output.exists())

    def test_stems_capabilities_opt_in_assets_and_rollback(self):
        self.extended_bundle()
        config = toolkit.configuration({'RB_TOOLKIT_STEMS':'1'})
        self.assertEqual(config['stems_dir'],'auto')
        result = self.launch_block(0,0,{'RB_TOOLKIT_STEMS':'1'})
        self.assertEqual(result.returncode,0,result.stderr)
        text = (self.chroot/'tmp/rx3-toolkit.env').read_text()
        self.assertIn('export RX3_STEMS_DIR=auto',text)
        self.assertIn('/root/pdj/rx3-stems-selected.rgb565',text)
        self.assertIn('RX3_TAB_LIGHT_10=/root/pdj/rx3-single-stems-selected-light.rgb565',text)
        env = toolkit.launch_environment(toolkit.configuration({}),'/root/pdj/rbp')
        self.assertEqual(env['RX3_STEMS_DIR'],'')
        self.assertEqual(env['RX3_TAB_DARK_01'],'')
        self.assertEqual(env['RX3_TAB_DARK_09'],'')
        self.assertEqual(self.prepare(0,0).read_bytes(),self.original)
        self.manifest['modules']=['keyshift']
        self.save()
        with self.assertRaisesRegex(ValueError,'rebuilt bundle'):
            toolkit.prepare(self.bundle,self.chroot,'/root/pdj/rbp',0,0,config)

    def test_stems_explicit_directory_is_scoped_to_chroot(self):
        self.extended_bundle()
        config = toolkit.configuration({'RB_TOOLKIT_STEMS':'1','RB_TOOLKIT_STEMS_DIR':'/root/pdj/RX3_STEMS'})
        with contextlib.redirect_stdout(io.StringIO()):
            toolkit.prepare(self.bundle,self.chroot,'/root/pdj/rbp',0,0,config)
        config['stems_dir']='/../../escape'
        with self.assertRaisesRegex(ValueError,'escapes'):
            toolkit.prepare(self.bundle,self.chroot,'/root/pdj/rbp',0,0,config)

    @unittest.skipUnless(shutil.which('cc'), 'requires host compiler')
    def test_stems_track_paths_two_usb_slots_reconnect_and_bounds(self):
        source = self.root/'paths.c'
        source.write_text(r'''#include <assert.h>
#include <string.h>
#include "pi_stems.h"
int main(void) {
    char out[1024];
#define PATH(track,dir,want) assert(!rx3_pi_stem_path(track,dir,out,sizeof out));assert(!strcmp(out,want))
    PATH("/media/usb1/sda1/Contents/Song.mp3","auto","/media/usb1/sda1/RX3_STEMS/Song.rx3stem");
    PATH("/media/usb2/sda1/Contents/Song.mp3","auto","/media/usb2/sda1/RX3_STEMS/Song.rx3stem");
    PATH("/media/usb1/sdb2/Contents/Song.wav","auto","/media/usb1/sdb2/RX3_STEMS/Song.rx3stem");
    PATH("/media/usb1/sda1/Contents/Música.v2.aiff","auto","/media/usb1/sda1/RX3_STEMS/Música.v2.rx3stem");
    PATH("/other/path/Song.wav","/root/pdj/stems/","/root/pdj/stems/Song.rx3stem");
    assert(rx3_pi_stem_path("/media/usb10/sda1/Song.mp3","auto",out,sizeof out)==-1);
    assert(rx3_pi_stem_path("/unrelated/Song.mp3","auto",out,sizeof out)==-1);
    assert(rx3_pi_stem_path("/media/usb1//Song.mp3","auto",out,sizeof out)==-1);
    assert(rx3_pi_stem_path("/media/usb1/sda1/","auto",out,sizeof out)==-1);
    strcpy(out,"untouched");
    assert(rx3_pi_stem_path("/media/usb1/sda1/Song.mp3","auto",out,8)==-1);
    assert(!strcmp(out,"untouched"));
    assert(rx3_pi_stem_path(0,"auto",out,sizeof out)==-1);
    return 0;
}
''')
        executable=self.root/'paths'
        subprocess.run(['cc','-Wall','-Wextra','-Werror','-I'+str(HERE.parent/'toolkit'),str(source),'-o',str(executable)],check=True)
        subprocess.run([str(executable)],check=True)

    @unittest.skipUnless(shutil.which('cc'), 'requires host compiler')
    def test_pi_input_bridge_pads_modes_shift_and_releases(self):
        source = self.root / 'input.c'
        source.write_text(r'''#include <assert.h>
#include "core/api/rx3_input_api.h"
#define KEY_CHANNELS 4u
struct pad_client { rx3_pad_handler handler; };
struct key_client { rx3_key_handler handler; };
static struct pad_client pad_clients[4];
static struct key_client key_clients[4];
static unsigned int pad_client_count, key_client_count;
static unsigned char shift_held[KEY_CHANNELS];
static void (*mode_key_action)(void);
#include "pi_input.inc"
static unsigned int keys, modes, presses, releases, sounding, stem_modes;
static void mode(void) { modes++; }
static int key(const struct rx3_key_event *event) {
    assert(event->key==RX3_KEY_SHIFT);keys++;return 0;
}
static int pad(const struct rx3_pad_event *event) {
    assert(event->deck==1 && event->channel==2 && event->code==0x411eu);
    if(event->pad_mode==2) {stem_modes++;return 1;}
    if(event->pad_mode!=0) return 0;
    if(event->operation==0) {presses++;sounding=1;}
    if(event->operation==2) {releases++;sounding=0;}
    return 1;
}
int main(void) {
    pad_client_count=key_client_count=1;
    pad_clients[0].handler=pad;key_clients[0].handler=key;mode_key_action=mode;
    assert(!rx3_pi_control(RX3_KEY_SHIFT,0,2));
    assert(keys==1 && shift_held[2]);
    assert(rx3_pi_control(0x411e,0,2) && presses==1 && sounding);
    assert(!rx3_pi_control(0x4113,0,2) && modes==1);
    assert(rx3_pi_control(0x411e,2,2) && releases==1 && !sounding);
    assert(!rx3_pi_control(RX3_KEY_SHIFT,2,2));
    assert(keys==2 && !shift_held[2]);
    assert(!rx3_pi_control(0x411e,0,0));
    assert(!rx3_pi_control(0x4101,0,2));
    assert(rx3_pi_pad(0x411e,0,2,2) && stem_modes==1);
    assert(rx3_pi_pad(0x411e,2,2,2) && stem_modes==2);
    assert(!rx3_pi_pad(0x411e,0,2,6));
    return 0;
}
''')
        # The public event types are copied locally for the host fixture so the
        # test remains runnable without an ignored third-party checkout.
        public = self.root / 'core/api/rx3_input_api.h'
        public.parent.mkdir(parents=True)
        public.write_text('''struct rx3_pad_event {unsigned int code,operation,channel,deck,pad_mode;};
struct rx3_key_event {unsigned int key,operation,channel;};
typedef int (*rx3_pad_handler)(const struct rx3_pad_event *);
typedef int (*rx3_key_handler)(const struct rx3_key_event *);
#define RX3_KEY_SHIFT 0x4103u
''')
        target = self.root / 'input'
        subprocess.run(['cc','-Wall','-Wextra','-Werror', '-I'+str(self.root),
                        '-I'+str(HERE.parent/'toolkit'),str(source),'-o',str(target)], check=True)
        subprocess.run([str(target)], check=True)

    @unittest.skipUnless(shutil.which('cc'), 'requires host compiler')
    def test_now_playing_ipv4_network_bytes_and_rejected_inputs(self):
        source = self.root / 'network.c'
        source.write_text(r'''#include <assert.h>
#include "pi_network.h"
int main(void) {
    assert(rx3_pi_ipv4("192.168.1.25")==0x1901a8c0u);
    assert(rx3_pi_ipv4("127.0.0.1")==0x0100007fu);
    assert(rx3_pi_ipv4("255.255.255.255")==0xffffffffu);
    assert(rx3_pi_ipv4("1.2.3.256")==0x0100007fu);
    assert(rx3_pi_ipv4("1.2.3.4.bad")==0x0100007fu);
    assert(rx3_pi_ipv4("1.2.3")==0x0100007fu);
    assert(rx3_pi_ipv4(0)==0x0100007fu);
    return 0;
}
''')
        executable = self.root / 'network'
        subprocess.run(['cc','-Wall','-Wextra','-Werror', '-I'+str(HERE.parent/'toolkit'),
                        str(source),'-o',str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

    @unittest.skipUnless(shutil.which('cc') and shutil.which('readelf') and
                         Path('/proc/self/cmdline').exists(), 'requires Linux host compiler/readelf')
    def test_process_guard_under_real_explicit_loader_and_inherited_utility(self):
        source = self.root / 'guard.c'
        source.write_text(r'''
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pi_process.h"
__attribute__((constructor)) static void initialize(void) {
    char args[512]; int fd=open("/proc/self/cmdline",O_RDONLY);
    if(fd<0)return;
    int n=read(fd,args,sizeof args);close(fd);
    if(n<=0 || !rx3_pi_player_cmdline(args,n,getenv("RX3_PI_PLAYER"),getenv("RX3_PI_LOADER")))return;
    fd=open(getenv("RX3_TEST_READY"),O_WRONLY|O_CREAT|O_TRUNC,0600);
    assert(fd>=0);assert(write(fd,"ready",5)==5);close(fd);
}
''')
        include = HERE.parent / 'toolkit'
        library = self.root / 'guard.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(include), str(source), '-o', str(library)], check=True)
        main = self.root / 'main.c'
        main.write_text('int main(void) { return 0; }\n')
        player = self.root / 'rbp'
        subprocess.run(['cc', str(main), '-o', str(player)], check=True)
        program = subprocess.check_output(['readelf', '-l', str(player)], text=True)
        loader = re.search(r'Requesting program interpreter: ([^]]+)', program).group(1)
        marker = self.root / 'ready'
        env = dict(os.environ, LD_PRELOAD=str(library), RX3_PI_PLAYER=str(player),
                   RX3_PI_LOADER=loader, RX3_TEST_READY=str(marker))
        for command in ([str(player)], [loader, str(player)]):
            subprocess.run(command, env=env, check=True)
            self.assertEqual(marker.read_text(), 'ready')
            marker.unlink()
        subprocess.run([shutil.which('true')], env=env, check=True)
        self.assertFalse(marker.exists())
        env['RX3_PI_PLAYER'] += '.wrong'
        subprocess.run([loader, str(player)], env=env, check=True)
        self.assertFalse(marker.exists())

    @unittest.skipUnless(shutil.which('cc'), 'requires host compiler')
    def test_process_guard_exact_arguments_and_complete_input(self):
        source = self.root / 'args.c'
        source.write_text(r'''
#include <assert.h>
#include <string.h>
#include "pi_process.h"
#define CHECK(args,want) assert(rx3_pi_player_cmdline(args,sizeof(args)-1,"/root/pdj/rbp.toolkit/rbp","/lib/ld-linux.so.3")==want)
int main(void) {
    CHECK("/root/pdj/rbp.toolkit/rbp\0-a\0",1);
    CHECK("/lib/ld-linux.so.3\0/root/pdj/rbp.toolkit/rbp\0-a\0",1);
    CHECK("/bin/grep\0/root/pdj/rbp.toolkit/rbp\0",0);
    CHECK("/bin/sh\0-c\0/root/pdj/rbp.toolkit/rbp\0",0);
    CHECK("/lib/ld-linux.so.3\0/usr/bin/edb_streamd\0",0);
    CHECK("/lib/ld-linux.so.3\0/root/pdj/rbp.toolkit/rbp.bad\0",0);
    CHECK("/lib/ld-linux.so.3.bad\0/root/pdj/rbp.toolkit/rbp\0",0);
    CHECK("/lib/ld-linux.so.3\0/root/pdj/rbp.toolkit/rbp",0);
    CHECK("",0);
    assert(!rx3_pi_player_cmdline("x\0",2,0,0));
    return 0;
}
''')
        target = self.root / 'args'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I' + str(HERE.parent / 'toolkit'),
                        str(source), '-o', str(target)], check=True)
        subprocess.run([str(target)], check=True)


if __name__ == '__main__':
    unittest.main()
