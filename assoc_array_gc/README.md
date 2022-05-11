assoc_array_gc bug
==================

A core dump was provided along with the error:

```
[3430308.818153] kernel BUG at lib/assoc_array.c:1609!
...
[3430309.673600] CPU: 22 PID: 257097 Comm: kworker/22:2 Tainted: P           O    4.14.35-1902.306.2.13.el7uek.x86_64 #2
[3430309.685647] Hardware name: Oracle Corporation ORACLE SERVER X6-2/ASM,MOTHERBOARD,1U, BIOS 38350200 06/21/2021
[3430309.697062] Workqueue: events key_garbage_collector
[3430309.702813] task: ffff88b7e396af80 task.stack: ffffab1fe37f4000
[3430309.709726] RIP: 0010:assoc_array_gc+0x4b0/0x4ec
[3430309.715304] RSP: 0018:ffffab1fe37f7d80 EFLAGS: 00010202
[3430309.721453] RAX: 0000000000000009 RBX: 0000000000000008 RCX: 0000000000000000
[3430309.729775] RDX: 0000000000000005 RSI: ffff88bad2ea7d41 RDI: ffff88bad2ea6780
[3430309.738072] RBP: ffffab1fe37f7de8 R08: ffff88bad2ea7680 R09: 00000001802a001f
[3430309.746367] R10: 00000000d2ea6001 R11: ffff88bad2ea7680 R12: 0000000000000009
[3430309.754661] R13: ffff88b5ca5ac9c0 R14: 0000000000000005 R15: 0000000000000005
[3430309.762958] FS:  0000000000000000(0000) GS:ffff88d4fea00000(0000) knlGS:0000000000000000
[3430309.772324] CS:  0010 DS: 0000 ES: 0000 CR0: 0000000080050033
[3430309.779037] CR2: 00007fe14d220000 CR3: 00000026f740a001 CR4: 00000000003606e0
[3430309.787343] DR0: 0000000000000000 DR1: 0000000000000000 DR2: 0000000000000000
[3430309.795680] DR3: 0000000000000000 DR6: 00000000fffe0ff0 DR7: 0000000000000400
[3430309.803976] Call Trace:
[3430309.807000]  ? __switch_to_asm+0x34/0x62
[3430309.811707]  ? keyring_destroy+0xd0/0xc7
[3430309.816409]  keyring_gc+0x6e/0x78
[3430309.820453]  key_garbage_collector+0x16a/0x3fd
[3430309.825711]  process_one_work+0x169/0x399
[3430309.830529]  worker_thread+0x4d/0x3e5
[3430309.834913]  kthread+0x105/0x138
[3430309.838824]  ? rescuer_thread+0x380/0x375
[3430309.843608]  ? kthread_bind+0x20/0x15
[3430309.847989]  ret_from_fork+0x3e/0x49
[3430309.852297] Code: 48 63 45 9c 0f 85 48 ff ff ff 49 89 df 48 8b 5d c0 49 c7 04 24 00 00 00 00 41 c7 44 24 08 00 00 00 00 eb a0 e8 32 0c ca ff 0f 0b <0f> 0b 49 89 df 4c 8b 75 c8 48 8b 5d c0 eb 8a 48 8b 5d c0 48 8b
[3430309.873771] RIP: assoc_array_gc+0x4b0/0x4ec RSP: ffffab1fe37f7d80
```

From the core dump stack, I retrieved the address of the `struct assoc_array` in
question. I created the following code:

- `repro.c`: several pieces of kernel declarations and the `assoc_array_gc`
  function itself, copied into userspace and made to work there.
- `assoc_array_constructor.py`: a script based on
  [drgn](https://github.com/osandov/drgn) which takes the address and generates
  some C code to build an equivalent assoc_array
- `construct_array.c`: output of the above, on the core dump
- `repro_out.txt`: result of running the repro

With my fix:

- `repro_fixed.c`: same code, but adding my patch
- `repro_fixed.txt`: output of running the above, successful this time!
