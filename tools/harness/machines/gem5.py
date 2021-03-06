##########################################################################
# Copyright (c) 2012-2016 ETH Zurich.
# All rights reserved.
#
# This file is distributed under the terms in the attached LICENSE file.
# If you do not find this file, copies can be found by writing to:
# ETH Zurich D-INFK, Universitaetstr 6, CH-8092 Zurich. Attn: Systems Group.
##########################################################################

# Quirks:
# * this is only running in single-core mode, since bootarm=0 is
#    used in above mentioned menu.lst

import os, signal, tempfile, subprocess, shutil, time
import debug, machines
from machines import Machine

GEM5_PATH = '/home/netos/tools/gem5/gem5-stable'
# gem5 takes quite a while to come up. If we return right away,
# telnet will be opened too early and fails to connect
GEM5_START_TIMEOUT = 5 # in seconds

IMAGE_NAME="armv7_a15ve_image"

class Gem5MachineBase(Machine):
    def __init__(self, options):
        super(Gem5MachineBase, self).__init__(options)
        self.child = None
        self.telnet = None
        self.tftp_dir = None
        self.options = options

    def get_buildall_target(self):
        return "VExpressEMM-A15"

    def get_coreids(self):
        return range(0, self.get_ncores())

    def get_tickrate(self):
        return None

    def get_boot_timeout(self):
        # we set this to 10 mins since gem5 is very slow
        return 600

    def get_test_timeout(self):
        # give gem5 tests enough time to complete
        # 20 mins
        return 15 * 60

    def get_machine_name(self):
        return self.name

    def force_write(self, consolectrl):
        pass

    def get_tftp_dir(self):
        if self.tftp_dir is None:
            debug.verbose('creating temporary directory for Gem5 files')
            self.tftp_dir = tempfile.mkdtemp(prefix='harness_gem5_')
            debug.verbose('Gem5 install directory is %s' % self.tftp_dir)
        return self.tftp_dir

    # Use menu.lst in hake/menu.lst.arm_gem5
    def _write_menu_lst(self, data, path):
        pass

    def set_bootmodules(self, modules):
        pass

    def lock(self):
        pass

    def unlock(self):
        pass

    def setup(self, builddir=None):
        self.builddir = builddir

    def _get_cmdline(self):
        raise NotImplementedError

    def get_kernel_args(self):
        # gem5 needs periphbase as argument as it does not implement the cp15
        # CBAR register
        return [ "periphbase=0x2c000000" ]

    def _kill_child(self):
        # terminate child if running
        if self.child:
            try:
                os.kill(self.child.pid, signal.SIGTERM)
            except OSError, e:
                debug.verbose("Caught OSError trying to kill child: %r" % e)
            except Exception, e:
                debug.verbose("Caught exception trying to kill child: %r" % e)
            try:
                self.child.wait()
            except Exception, e:
                debug.verbose("Caught exception while waiting for child: %r" % e)
            self.child = None

    def reboot(self):
        self._kill_child()
        cmd = self._get_cmdline()
        debug.verbose('starting "%s" in gem5.py:reboot' % ' '.join(cmd))
        devnull = open('/dev/null', 'w')
        # remove ubuntu chroot from environment to make sure gem5 finds the
        # right shared libraries
        import os
        env = dict(os.environ)
        if 'LD_LIBRARY_PATH' in env:
            del env['LD_LIBRARY_PATH']
        self.child = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=devnull, env=env)
        time.sleep(GEM5_START_TIMEOUT)

    def shutdown(self):
        debug.verbose('gem5:shutdown requested');
        debug.verbose('terminating gem5')
        if not self.child is None:
            try:
                self.child.terminate()
            except OSError, e:
                debug.verbose("Error when trying to terminate gem5: %r" % e)
        debug.verbose('closing telnet connection')
        if not self.telnet is None:
            self.output.close()
            self.telnet.close()
        # try to cleanup tftp tree if needed
        if self.tftp_dir and os.path.isdir(self.tftp_dir):
            shutil.rmtree(self.tftp_dir, ignore_errors=True)
        self.tftp_dir = None

    def get_output(self):
        # wait a bit to give gem5 time to listen for a telnet connection
        if self.child.poll() != None: # Check if child is down
            print 'gem5 is down, return code is %d' % self.child.returncode
            return None
        # use telnetlib
        import telnetlib
        self.telnet_connected = False
        while not self.telnet_connected:
            try:
                self.telnet = telnetlib.Telnet("localhost", self.telnet_port)
                self.telnet_connected = True
                self.output = self.telnet.get_socket().makefile()
            except IOError, e:
                errno, msg = e
                if errno != 111: # connection refused
                    debug.error("telnet: %s [%d]" % (msg, errno))
                else:
                    self.telnet_connected = False
            time.sleep(GEM5_START_TIMEOUT)

        return self.output

class Gem5MachineARM(Gem5MachineBase):
    def get_bootarch(self):
        return 'armv7'

    def get_platform(self):
        return 'a15ve'

    def set_bootmodules(self, modules):
        # store path to kernel for _get_cmdline to use
        self.kernel_img = os.path.join(self.options.buildbase,
                                       self.options.builds[0].name,
                                       IMAGE_NAME)

        #write menu.lst
        path = os.path.join(self.get_tftp_dir(), 'menu.lst')
        self._write_menu_lst(modules.get_menu_data('/'), path)

# SK: did not test this yet, but should work
# @machines.add_machine
# class Gem5MachineARMSingleCore(Gem5MachineARM):
#     name = 'gem5_arm_1'

#     def get_ncores(self):
#         return 1

#     def _get_cmdline(self):
#         script_path = os.path.join(self.options.sourcedir, 'tools/arm_gem5', 'gem5script.py')
#         return (['gem5.fast', script_path, '--kernel=%s'%self.kernel_img, '--n=%s'%self.get_ncores()]
#                 + GEM5_CACHES_ENABLE)

@machines.add_machine
class Gem5MachineARMSingleCore(Gem5MachineARM):
    name = 'armv7_gem5'

    def get_bootarch(self):
        return "armv7"

    def get_platform(self):
        return 'a15ve'

    def get_ncores(self):
        return 1

    def get_cores_per_socket(self):
        return 1

    def setup(self, builddir=None):
        self.builddir = builddir

    def get_free_port(self):
        import socket
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.bind(('', 0))
        # extract port from addrinfo
        self.telnet_port = s.getsockname()[1]
        s.close()

    def _write_menu_lst(self, data, path):
        debug.verbose('writing %s' % path)
        debug.debug(data)
        f = open(path, 'w')
        f.write(data)
        # TODO: provide mmap properly somehwere (machine data?)
        f.write("mmap map 0x80000000 0x20000000 1\n")
        f.close()

    def set_bootmodules(self, modules):
        super(Gem5MachineARMSingleCore, self).set_bootmodules(modules)
        debug.verbose("writing menu.lst in build directory")
        menulst_fullpath = os.path.join(self.builddir,
                "platforms", "arm", "menu.lst.armv7_a15ve")
        debug.verbose("writing menu.lst in build directory: %s" %
                menulst_fullpath)
        self._write_menu_lst(modules.get_menu_data("/"), menulst_fullpath)
        debug.verbose("building proper gem5 image")
        debug.checkcmd(["make", IMAGE_NAME], cwd=self.builddir)

    def _get_cmdline(self):
        self.get_free_port()
        script_path = \
            os.path.join(self.options.sourcedir, 'tools/arm_gem5',
                         'boot_gem5.sh')
        return ([script_path, 'VExpress_EMM', self.kernel_img, GEM5_PATH,
                 str(self.telnet_port)])
