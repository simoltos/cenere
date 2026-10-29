#!/usr/bin/env python3
"""Pure storage planning and package-contract regressions; no disk operations."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]
DRIVER = r'''
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/stat.h>
int intercepted_execv(const char *, char *const []);
int intercepted_stat(const char *, struct stat *);
#define execv intercepted_execv
#define stat(path, result) intercepted_stat(path, result)
#include "cenere_install.c"
#undef execv
#undef stat
int intercepted_stat(const char *path, struct stat *result)
{
    if (!strcmp(path,"/dev/cenere-test") || !strncmp(path,"/dev/disk/by-uuid/",18)) {
        bool alias=strcmp(path,"/dev/cenere-test");
        const char *mode=getenv("CENERE_TEST_ALIAS");
        if (alias && mode && !strcmp(mode,"missing")) { errno=ENOENT; return -1; }
        *result=(struct stat){.st_mode=S_IFBLK|0600,.st_rdev=42};
        if (alias && mode && !strcmp(mode,"other")) result->st_rdev=43;
        if (alias && mode && !strcmp(mode,"regular")) result->st_mode=S_IFREG|0600;
        return 0;
    }
    return stat(path,result);
}
int intercepted_execv(const char *path, char *const args[])
{
    const char *shrink=getenv("CENERE_TEST_SHRINK_FAILURE");
    if (shrink) {
        FILE *calls=fopen(getenv("CENERE_TEST_CALLS"),"a");
        if (!calls) _exit(98);
        for (size_t i=0;args[i];i++) fprintf(calls,"%s%s",i?" ":"",args[i]);
        fputc('\n',calls);fclose(calls);
        if (!strcmp(path,"/usr/bin/sfdisk") && !strcmp(args[1],"--dump")) {
            puts("label: gpt\n/dev/example1 : start=2048, size=83886080, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4");
            fflush(stdout);_exit(0);
        }
        if (!strcmp(path,"/usr/bin/e2fsck")) _exit(!strcmp(shrink,"check")?4:0);
        if (!strcmp(path,"/usr/bin/resize2fs")) _exit(!strcmp(shrink,"resize")?1:0);
        if (!strcmp(path,"/usr/bin/dumpe2fs")) {
            puts("Block count: 33554432\nBlock size: 4096\n");fflush(stdout);_exit(0);
        }
        if (!strcmp(path,"/usr/bin/ntfsresize"))
            _exit(!strcmp(shrink,"ntfs-check") || strcmp(args[1],"--no-action")?1:0);
        _exit(99);
    }
    if (!strcmp(path,"/usr/bin/blkid")) {
        if (strcmp(args[1],"--probe")) _exit(99);
        const char *identity=getenv("CENERE_TEST_IDENTITY"), *status=getenv("CENERE_TEST_PROBE_STATUS");
        if (identity) fputs(identity,stdout);
        fflush(stdout);
        _exit(status?atoi(status):0);
    }
    if (!strcmp(path, "/usr/bin/arch-chroot") || !strcmp(path,"/usr/bin/wipefs")) {
        for (size_t i=0; args[i]; i++) puts(args[i]);
        fflush(stdout);
        _exit(getenv("CENERE_TEST_WIPE_FAIL")?1:0);
    }
    /* These file operations use disposable fixtures; no disk utility executes. */
    if (!strcmp(path,"/usr/bin/rsync") && getenv("CENERE_TEST_RSYNC_FAIL")) _exit(37);
    if (!strcmp(path,"/usr/bin/du") || !strcmp(path,"/usr/bin/rsync")) return execv(path,args);
    _exit(99);
}
int main(int argc, char **argv)
{
    if (argc!=3) return 2;
    if (!strcmp(argv[1],"shrink-failure")) {
        snprintf(work,sizeof(work),"%s",argv[2]);
        struct storage s={.sector=512,.count=1,.shrink=0,.gpt=true};
        struct installation_plan p={0};
        snprintf(p.target,sizeof(p.target),"/dev/example");
        s.parts[0]=(struct partition){.start=2048,.size=40*GiB/512,.original_size=60*GiB/512,.keep=true};
        snprintf(s.parts[0].device,sizeof(s.parts[0].device),"/dev/example1");
        snprintf(s.parts[0].fs,sizeof(s.parts[0].fs),"%s",
                 !strncmp(getenv("CENERE_TEST_SHRINK_FAILURE"),"ntfs",4)?"ntfs":"ext4");
        bool ok=partition_disk(&s,&p);
        if (!ok) fprintf(stderr,"%s\n",cenere_error());
        return ok?0:1;
    }
    if (!strcmp(argv[1],"uuid")) {
        char value[128];
        bool ok=uuid("/dev/cenere-test",argv[2],value,sizeof(value));
        if (ok) puts(value); else fprintf(stderr,"%s\n",cenere_error());
        return ok?0:1;
    }
    if (!strcmp(argv[1],"clear-partition")) {
        struct partition p={.keep=true};
        snprintf(p.device,sizeof(p.device),"/dev/cenere-test");
        if (!strcmp(argv[2],"retained")) p.original_size=4096;
        if (!strcmp(argv[2],"deleted")) p.keep=false;
        bool ok=clear_new_partition(&p);
        if (!ok) fprintf(stderr,"%s\n",cenere_error());
        return ok?0:1;
    }
    if (!strcmp(argv[1],"manifest") || !strcmp(argv[1],"arguments")) {
        struct package_manifest final={0}, explicit={0}, live={0};
        bool ok=manifest_contract(argv[2],&final,&explicit,&live);
        if (ok && !strcmp(argv[1],"arguments"))
            ok=target_package_operation("/unused-target","-D","--asdeps",&final);
        free_manifest(&final); free_manifest(&explicit); free_manifest(&live);
        if (!ok) fprintf(stderr,"%s\n",cenere_error());
        return ok?0:1;
    }
    if (!strcmp(argv[1],"initramfs-config") || !strcmp(argv[1],"initramfs-plain")) {
        bool ok=configure_initramfs(argv[2],!strcmp(argv[1],"initramfs-config"));
        if (!ok) fprintf(stderr,"%s\n",cenere_error());
        return ok?0:1;
    }
    if (!strcmp(argv[1],"initramfs-list")) {
        char listing[8192]="";
        FILE *file=fopen(argv[2],"r");if (!file) return 2;
        fread(listing,1,sizeof(listing)-1,file);fclose(file);
        bool ok=initramfs_contract(listing,true);
        if (!ok) fprintf(stderr,"%s\n",cenere_error());
        return ok?0:1;
    }
    if (!strcmp(argv[1],"preserve-boot")) {
        char boot[PATH_MAX],backup[PATH_MAX];
        snprintf(boot,sizeof(boot),"%s/boot",argv[2]);
        snprintf(backup,sizeof(backup),"%s/backup",argv[2]);
        bool ok=preserve_boot_files(boot,backup);
        if (!ok) fprintf(stderr,"%s\n",cenere_error());
        return ok?0:1;
    }
    if (!strcmp(argv[1],"keyboards")) {
        struct cenere_keyboard maps[256];
        size_t count=cenere_keyboards(maps,256,true);
        for (size_t i=0;i<count;i++) printf("%s|%s|%s|%s|%s\n",maps[i].keymap,maps[i].layout,
                                          maps[i].variant,maps[i].name,maps[i].variant_name);
        return 0;
    }
    struct installation_plan p={0};
    snprintf(p.source,sizeof(p.source),"%s",argv[2]);
    snprintf(p.target,sizeof(p.target),"/dev/example");
    snprintf(p.firmware,sizeof(p.firmware),"uefi");
    struct storage s={.sector=512,.bytes=64*GiB,.first=2048,.last=64*GiB/512-34,
        .esp=-1,.bios=-1,.root=-1,.home=-1,.source_home=-1,.boot=-1,.old_boot=-1,.shrink=-1};
    if (strcmp(argv[1],"fresh")) {
        s.gpt=true;
        s.bios=append_partition(&s,p.target,2048,2048,BIOS_TYPE);
        s.esp=append_partition(&s,p.target,4096,512*MiB/512,ESP_TYPE);
        s.old_boot=append_partition(&s,p.target,1052672,2*GiB/512,LINUX_TYPE);
        s.home=append_partition(&s,p.target,5246976,24*GiB/512,LINUX_TYPE);
        s.source_home=s.home;
        for (size_t i=0;i<s.count;i++) {
            s.parts[i].original_size=s.parts[i].size;
            snprintf(s.parts[i].uuid,sizeof(s.parts[i].uuid),"existing-%zu",i);
        }
        p.preserve_home=true;
        s.home_used=1*GiB;
        s.home_measured=true;
        s.system_available=20*GiB;
        s.root_id=256; s.home_id=257;
        snprintf(s.root_subvol,sizeof(s.root_subvol),"@");
        snprintf(s.home_subvol,sizeof(s.home_subvol),"@home");
        if (!strcmp(argv[1],"encrypted")) p.encrypt=s.encrypted_home=true;
        if (!strcmp(argv[1],"change-encryption")) p.encrypt=true;
        if (!strcmp(argv[1],"remove-encryption")) s.encrypted_home=true;
        if (!strcmp(argv[1],"foreign")) {
            snprintf(s.root_subvol,sizeof(s.root_subvol),"system-volume");
            snprintf(s.home_subvol,sizeof(s.home_subvol),"personal-data");
        }
        if (!strcmp(argv[1],"inline-copy")) s.inline_home=true;
        if (!strcmp(argv[1],"unsupported-home")) s.home=s.source_home=-1;
        if (!strcmp(argv[1],"missing-boot")) s.old_boot=-1;
        if (!strcmp(argv[1],"mbr-preservation")) s.gpt=false;
        if (!strcmp(argv[1],"alongside-free")) p.alongside=true;
        if (!strcmp(argv[1],"fedora-full-disk")) {
            s.count=0;s.bios=-1;s.bytes=512110190592ULL;s.last=s.bytes/512-34;
            s.esp=append_partition(&s,p.target,2048,1228800,ESP_TYPE);
            s.old_boot=append_partition(&s,p.target,1230848,4194304,LINUX_TYPE);
            s.home=append_partition(&s,p.target,5425152,994789376,LINUX_TYPE);
            s.source_home=s.home;s.home_used=170*GiB;s.system_available=296*GiB;
            p.encrypt=s.encrypted_home=true;
            snprintf(s.root_subvol,sizeof(s.root_subvol),"root");
            snprintf(s.home_subvol,sizeof(s.home_subvol),"home");
        }
        if (!strcmp(argv[1],"ambiguous")) s.ambiguous_home=true;
        if (!strcmp(argv[1],"full")) s.system_available=0;
    }
    if (!plan_storage(&s,&p)) { fprintf(stderr,"%s\n",cenere_error()); return 1; }
    printf("shared=%d reuse=%d count=%zu source-kept=%d\n",s.root==s.home,s.reuse_btrfs,
           s.count,s.source_home>=0 && s.parts[s.source_home].keep);
    puts(p.layout);
    return 0;
}
'''


class StorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (PROJECT / "test-results").mkdir(exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix="storage-", dir=PROJECT / "test-results")
        cls.base = Path(cls.temporary.name)
        cls.driver = cls.base / "driver"
        source = cls.base / "driver.c"
        source.write_text(DRIVER)
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2",
                        *shlex.split(os.environ.get("CPPFLAGS", "")), "-I", str(PROJECT),
                        str(source), "-o", str(cls.driver),
                        *shlex.split(os.environ.get("LDLIBS", "-ljson-c"))],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def invoke(self, mode, source, success=True, extra_env=None):
        result = subprocess.run([str(self.driver), mode, str(source)], capture_output=True,
                                text=True, timeout=10, env={**os.environ, **(extra_env or {})})
        self.assertEqual(result.returncode, 0 if success else 1, result.stderr)
        return result.stdout + result.stderr

    def manifest(self):
        temp = tempfile.TemporaryDirectory(dir=self.base)
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        folder = root / "usr/share/cenere"
        folder.mkdir(parents=True)
        (folder / "packages.final").write_text("base 3-2\ngrub 2-1\nmkinitcpio 1-1\npacman 7-1\nshared-library 1-1\nsystemd 1-1\n")
        (folder / "packages.explicit").write_text("base\n")
        (folder / "packages.live-only").write_text("arch-install-scripts\ncenere\nrsync\n")
        return root, folder

    def test_fresh_root_and_home_share_one_partition(self):
        result = self.invoke("fresh", self.base)
        self.assertIn("shared=1 reuse=0 count=4", result)
        self.assertEqual(result.count("Btrfs system and home"), 1)

    def test_reinstall_retains_partitions_home_ids_and_encryption(self):
        for mode in ("reuse", "encrypted"):
            with self.subTest(mode=mode):
                result = self.invoke(mode, self.base)
                self.assertIn("shared=1 reuse=1 count=4 source-kept=1", result)
                self.assertIn("KEEP home subvolume @home (ID 257)", result)
                self.assertNotIn("CREATE", result)

    def test_other_layouts_copy_home_without_reusing_or_formatting_source(self):
        for mode in ("inline-copy", "change-encryption", "remove-encryption"):
            with self.subTest(mode=mode):
                result = self.invoke(mode, self.base)
                self.assertIn("shared=1 reuse=0 count=6 source-kept=1", result)
                self.assertIn("source home retained; files copied and verified", result)

    def test_btrfs_reuse_is_independent_of_distribution_and_subvolume_names(self):
        result=self.invoke("foreign",self.base)
        self.assertIn("reuse=1",result)
        self.assertIn("KEEP home subvolume personal-data",result)
        result=self.invoke("fedora-full-disk",self.base)
        self.assertIn("shared=1 reuse=1 count=3 source-kept=1",result)
        self.assertIn("KEEP home subvolume home",result)
        self.assertNotIn("CREATE",result)

    def test_unsupported_home_is_rejected_before_planning_changes(self):
        output=self.invoke("unsupported-home",self.base,success=False)
        self.assertIn("compatible Btrfs layout",output)

    def test_reuse_requires_a_boot_partition_and_gpt(self):
        for mode in ("missing-boot","mbr-preservation"):
            with self.subTest(mode=mode):
                self.invoke(mode,self.base,success=False)

    def test_alongside_keeps_original_home_and_uses_shared_btrfs_destination(self):
        result=self.invoke("alongside-free",self.base)
        self.assertIn("shared=1 reuse=0",result)
        self.assertIn("source-kept=1",result)
        self.assertNotIn("DELETE",result)
        self.assertIn("files copied and verified",result)

    def test_only_new_confirmed_partitions_can_have_signatures_erased(self):
        output=self.invoke("clear-partition","new")
        self.assertIn("/usr/bin/wipefs\n--all\n--force\n--lock=yes\n--\n/dev/cenere-test",output)
        for state in ("retained","deleted"):
            output=self.invoke("clear-partition",state,success=False)
            self.assertNotIn("/usr/bin/wipefs",output)
            self.assertIn("Refusing to erase",output)

    def test_failed_shrink_never_reduces_partition_boundaries(self):
        for failure in ('check','resize','geometry','ntfs-check','ntfs-resize'):
            with self.subTest(failure=failure):
                calls=self.base/(failure+'.calls')
                self.invoke('shrink-failure',self.base,success=False,extra_env={
                    'CENERE_TEST_SHRINK_FAILURE':failure,'CENERE_TEST_CALLS':str(calls)})
                commands=calls.read_text()
                self.assertIn('sfdisk --dump',commands)
                self.assertNotIn('sfdisk --lock',commands)
                if failure=='check':
                    self.assertNotIn('resize2fs',commands)
                if failure=='ntfs-check':
                    self.assertNotIn('ntfsresize --size',commands)

    def test_signature_erasure_failure_stops_preparation(self):
        self.invoke("clear-partition","new",success=False,extra_env={"CENERE_TEST_WIPE_FAIL":"1"})

    def test_uncached_uuid_requires_expected_filesystem_and_device(self):
        for kind,identity in (("btrfs","8d7da79c-85a0-4054-9e85-20400e6e4dc2"),
                              ("crypto_LUKS","41c8184d-e1ac-4673-bf32-38b16c4b6f67"),
                              ("ext4","56fb8c72-73ab-403f-8216-5a1aa86c507e"),
                              ("vfat","DDBA-C96B")):
            with self.subTest(kind=kind):
                output=self.invoke("uuid",kind,extra_env={"CENERE_TEST_IDENTITY":f"DEVNAME=/dev/cenere-test\nUUID={identity}\nTYPE={kind}\n"})
                self.assertEqual(output.strip(),identity)

    def test_old_luks_signature_and_ambiguous_or_incomplete_probes_are_rejected(self):
        for identity,status in (("UUID=41c8184d-e1ac-4673-bf32-38b16c4b6f67\nTYPE=crypto_LUKS\n","0"),
                                ("UUID=1234-abcd\nTYPE=btrfs\n","8"),
                                ("TYPE=btrfs\n","0"),("UUID=1234-abcd\n","0"),
                                ("UUID=1234-abcd\nUUID=5678-abcd\nTYPE=btrfs\n","0"),
                                ("DEVNAME=/dev/other\nUUID=1234-abcd\nTYPE=btrfs\n","0"),
                                ("UUID=unsafe UUID\nTYPE=btrfs\n","0")):
            with self.subTest(identity=identity,status=status):
                self.invoke("uuid","btrfs",success=False,extra_env={"CENERE_TEST_IDENTITY":identity,
                            "CENERE_TEST_PROBE_STATUS":status})

    def test_uuid_link_must_resolve_to_the_selected_block_device(self):
        for alias in ("missing","other","regular"):
            with self.subTest(alias=alias):
                output=self.invoke("uuid","btrfs",success=False,extra_env={
                    "CENERE_TEST_IDENTITY":"UUID=1234-abcd\nTYPE=btrfs\n","CENERE_TEST_ALIAS":alias})
                self.assertIn("does not resolve",output)

    def test_ambiguous_home_and_full_shared_pool_are_rejected(self):
        for mode in ("ambiguous", "full"):
            with self.subTest(mode=mode):
                self.invoke(mode, self.base, success=False)

    def test_keyboard_catalog_uses_standard_mapping_and_descriptions(self):
        mapping=self.base/"kbd-model-map"
        names=self.base/"evdev.lst"
        mapping.write_text("# fixture\nit2 it pc105 - -\nit it pc105 - -\nit-us it pc105 us -\nunsafe;name us pc105 - -\nctrl\n")
        names.write_text("! layout\n  it Italian\n! variant\n  us it: Italian (US keyboard)\n")
        result=subprocess.run([str(self.driver),"keyboards",str(self.base)],capture_output=True,text=True,
            env={**os.environ,"CENERE_KEYBOARD_MAP":str(mapping),"CENERE_XKB_NAMES":str(names)},check=True)
        self.assertEqual(result.stdout.splitlines(),["it|it||Italian|Default","it-us|it|us|Italian|Italian (US keyboard)"])

    def test_generic_contract_needs_no_desktop_or_privilege_tool(self):
        root,folder=self.manifest()
        self.invoke("manifest",root)
        self.assertNotIn("gnome",(folder/"packages.final").read_text())
        self.assertNotIn("opendoas",(folder/"packages.final").read_text())
        (folder/"packages.final").write_text("base 3-2\nmkinitcpio 1-1\npacman 7-1\nsystemd 1-1\n")
        self.invoke("manifest",root,success=False)

    def test_explicit_main_config_and_dropin_have_matching_hooks(self):
        with tempfile.TemporaryDirectory(dir=self.base) as temporary:
            root=Path(temporary);(root/"etc/mkinitcpio.conf.d").mkdir(parents=True)
            main=root/"etc/mkinitcpio.conf"
            main.write_text("MODULES=(btrfs)\nCOMPRESSION=zstd\nHOOKS=(base systemd filesystems)\n")
            for mode,encrypted in (("initramfs-config",True),("initramfs-plain",False)):
                self.invoke(mode,root)
                effective=subprocess.check_output(["bash","-c",'source "$1"; printf "%s\n" "${HOOKS[*]}" "${MODULES[*]}" "$COMPRESSION"',"test",str(main)],text=True).splitlines()
                self.assertEqual("sd-encrypt" in effective[0].split(),encrypted)
                self.assertIn("sd-vconsole",effective[0].split())
                self.assertEqual(effective[1:],["btrfs","zstd"])
                dropin=(root/"etc/mkinitcpio.conf.d/cenere.conf").read_text().strip()
                self.assertTrue(main.read_text().strip().endswith(dropin))

    def test_initramfs_configuration_refuses_main_symlinks(self):
        with tempfile.TemporaryDirectory(dir=self.base) as temporary:
            root=Path(temporary);(root/"etc/mkinitcpio.conf.d").mkdir(parents=True)
            outside=root/"unrelated";outside.write_text("preserve\n")
            (root/"etc/mkinitcpio.conf").symlink_to(outside)
            self.invoke("initramfs-config",root,success=False)
            self.assertEqual(outside.read_text(),"preserve\n")

    def test_generated_archive_needs_actual_unlock_and_keyboard_components(self):
        names=["usr/lib/systemd/systemd","usr/lib/systemd/systemd-vconsole-setup","usr/bin/loadkeys",
               "etc/vconsole.conf","usr/lib/systemd/systemd-cryptsetup",
               "usr/lib/systemd/system-generators/systemd-cryptsetup-generator","etc/crypttab"]
        listing=self.base/"initramfs-list"
        listing.write_text("".join("./"+n+"\n" for n in names))
        self.invoke("initramfs-list",listing)
        for missing in names:
            listing.write_text("".join(n+"\n" for n in names if n!=missing)+missing+"-wrong\n")
            self.invoke("initramfs-list",listing,success=False)

    def test_reused_boot_is_verified_before_kernel_retirement(self):
        for failure in (False,True):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory(dir=self.base) as temporary:
                root=Path(temporary);boot=root/"boot";boot.mkdir()
                kernels=["vmlinuz-linux","initramfs-linux.img","vmlinuz-6.18.1-300.fc43.x86_64",
                         "initramfs-6.18.1-300.fc43.x86_64.img","initrd.img-6.12-generic"]
                kernels += [f'vmlinuz-{index}-fixture' for index in range(80)]
                for name in [*kernels,"custom-data"]:(boot/name).write_bytes(name.encode())
                (boot/"efi/EFI/OtherOS").mkdir(parents=True)
                (boot/"efi/EFI/OtherOS/loader.efi").write_bytes(b"preserve EFI")
                (boot/"grub2").mkdir();(boot/"grub2/grub.cfg").write_bytes(b"previous boot configuration")
                before={p.relative_to(boot):p.read_bytes() for p in boot.rglob("*") if p.is_file()}
                env={"CENERE_TEST_RSYNC_FAIL":"1"} if failure else None
                self.invoke("preserve-boot",root,success=not failure,extra_env=env)
                after={p.relative_to(boot):p.read_bytes() for p in boot.rglob("*") if p.is_file()}
                expected=before if failure else {p:v for p,v in before.items() if str(p) not in kernels}
                self.assertEqual(after,expected)
                if not failure:
                    saved={p.relative_to(root/"backup"):p.read_bytes() for p in (root/"backup").rglob("*") if p.is_file()}
                    self.assertEqual(saved,{p:v for p,v in before.items() if p.parts[0]!="efi"})
                    self.invoke("preserve-boot",root,success=False)
                    self.assertEqual(after,{p.relative_to(boot):p.read_bytes() for p in boot.rglob("*") if p.is_file()})

    def test_valid_package_contract_retains_shared_dependency(self):
        root, _ = self.manifest()
        self.invoke("manifest", root)

    def test_overlap_installer_in_final_and_missing_explicit_are_rejected(self):
        final="base 3-2\ngrub 2-1\nmkinitcpio 1-1\npacman 7-1\nshared-library 1-1\nsystemd 1-1\n"
        for filename, content in (
            ("packages.live-only", "cenere\nshared-library\n"),
            ("packages.final", final.replace("grub 2-1", "cenere 1-1\ngrub 2-1")),
            ("packages.explicit", "base\nmissing\n"),
            ("packages.final", "base 3-2\n"+final),
            ("packages.final", final+"unsafe;name 1-1\n"),
        ):
            with self.subTest(filename=filename, content=content):
                root, folder = self.manifest()
                (folder / filename).write_text(content)
                self.invoke("manifest", root, success=False)

    def test_full_package_list_reaches_pacman_without_argument_truncation(self):
        root, folder = self.manifest()
        packages = sorted(["base", "grub", "mkinitcpio", "pacman", "systemd"] + [f"package-{i:04}" for i in range(700)])
        (folder / "packages.final").write_text("".join(f"{p} 1-1\n" for p in packages))
        result = self.invoke("arguments", root).splitlines()
        self.assertEqual(result[:6], ["/usr/bin/arch-chroot", "/unused-target",
                                    "/usr/bin/pacman", "-D", "--asdeps", "--"])
        self.assertEqual(result[6:], packages)


if __name__ == "__main__":
    unittest.main(verbosity=2)
