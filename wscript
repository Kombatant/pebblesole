#
# This file is the default set of rules to compile a Pebble application.
#
# Feel free to customize this to your needs.
#
import os.path
import os
import json
import datetime

top = '.'
out = 'build'


def _write_version_js(ctx):
    """Generate src/pkjs/version.js from package.json + today's date so the
    Clay settings page can show the running version and build date."""
    with open(ctx.path.find_node('package.json').abspath(), 'r') as f:
        pkg = json.load(f)
    info = {
        'version': pkg.get('version', '0.0.0'),
        'buildDate': datetime.date.today().isoformat(),
    }
    out_path = ctx.path.find_dir('src/pkjs').make_node('version.js').abspath()
    with open(out_path, 'w') as f:
        f.write('// Auto-generated from package.json by wscript. Do not edit.\n')
        f.write('module.exports = ' + json.dumps(info) + ';\n')


def options(ctx):
    ctx.load('pebble_sdk')


def configure(ctx):
    """
    This method is used to configure your build. ctx.load(`pebble_sdk`) automatically configures
    a build for each valid platform in `targetPlatforms`. Platform-specific configuration: add your
    change after calling ctx.load('pebble_sdk') and make sure to set the correct environment first.
    Universal configuration: add your change prior to calling ctx.load('pebble_sdk').
    """
    ctx.load('pebble_sdk')


def build(ctx):
    ctx.load('pebble_sdk')

    _write_version_js(ctx)

    build_worker = os.path.exists('worker_src')
    binaries = []

    cached_env = ctx.env
    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.env = ctx.all_envs[platform]
        ctx.set_group(ctx.env.PLATFORM_NAME)
        app_elf = '{}/pebble-app.elf'.format(ctx.env.BUILD_DIR)
        if os.environ.get('VISUAL_FIXTURE') == '1':
            ctx.env.CFLAGS = list(ctx.env.CFLAGS) + ['-DVISUAL_FIXTURE']
        if os.environ.get('VISUAL_FIXTURE_BT_OFF') == '1':
            ctx.env.CFLAGS = list(ctx.env.CFLAGS) + ['-DVISUAL_FIXTURE_BT_OFF']
        if os.environ.get('VISUAL_FIXTURE_QUIET') == '1':
            ctx.env.CFLAGS = list(ctx.env.CFLAGS) + ['-DVISUAL_FIXTURE_QUIET']
        ctx.pbl_build(source=ctx.path.ant_glob('src/c/**/*.c'), target=app_elf, bin_type='app')

        if build_worker:
            worker_elf = '{}/pebble-worker.elf'.format(ctx.env.BUILD_DIR)
            binaries.append({'platform': platform, 'app_elf': app_elf, 'worker_elf': worker_elf})
            ctx.pbl_build(source=ctx.path.ant_glob('worker_src/c/**/*.c'),
                          target=worker_elf,
                          bin_type='worker')
        else:
            binaries.append({'platform': platform, 'app_elf': app_elf})
    ctx.env = cached_env

    ctx.set_group('bundle')
    ctx.pbl_bundle(binaries=binaries,
                   js=ctx.path.ant_glob(['src/pkjs/**/*.js',
                                         'src/pkjs/**/*.json',
                                         'src/common/**/*.js']),
                   js_entry_file='src/pkjs/index.js')
