# APFS на запись: план и прогресс

Цель: `/private/var` — настоящий APFS-том на userdata, доступный на запись,
чтобы 48 ГиБ были видны и использовались, и `var_store` стал не нужен.

## Что было в начале

- Драйвер APFS в ядре (`xnu/files/bsd/miscfs/apfs/`) — только чтение:
  объектные карты, B-деревья, inode/каталоги/extent/xattr, дырки, сжатие, криптексы.
- `apfs_write.c` — замена байтов внутри существующего файла, без изменения длины.
- `userland/src/var_store.c` — архив `/private/var` в раздел `darwin`.
- Канал записи во флеш: `/dev/rufs_darwin`, только с блока 2 042 368.
- `tools/apfs/mkapfs.py` — сборка APFS-контейнера на ПК (space manager, checkpoint map, reaper).
- Проверки: `apfsck` (linux-apfs) через `check-image.sh`, `verify.py`, qemu-стенд,
  настоящий `fsck_apfs` на маке.

## Этапы

| # | что | сессий | состояние |
|---|---|---|---|
| 0 | структуры spaceman/checkpoint/transaction, read-only разбор spaceman | 0.5 | сделано |
| 1 | тестовый APFS-том, монтируется, считает свободное место | 1 | сделано (mkapfs) |
| 2 | transaction engine + коммит checkpoint | 2 | сделано |
| 3 | alloc/free, create/mkdir/unlink/rename/truncate, xattr, hardlink | 2 | всё, кроме hardlink и потоковых xattr |
| 4 | VFS-запись в `apfs_vnops.c`, writeback, mmap | 1 | сделано |
| 5 | канал записи на userdata (`rufs_userdata`) + проверка в лоадере | 1 | сделано (проверено на телефоне) |
| 6 | том Data в контейнере, `apfs_volume=...` → `/private/var`, `var_store` выключить | 1 | сделано (смонтирован на 48 ГБ UFS) |
| 7 | проверки: `fsck_apfs`, два прогона, установка IPA, обрыв питания | 1 | в процессе (отладка datamigrator / next_obj_id) |

## Прогресс

### 2026-11-07, этап 0: space manager

- `apfs_format.h`: структуры space manager (device, свободные очереди, chunk info,
  CIB/CAB, datazone), константы. `apfs_lib.c`: поиск space manager через checkpoint map,
  разбор, проверки геометрии, обход чанков, битовые карты, locate/block_used.
- `apfs-tool spaceman IMAGE [--chunks]`: печатает свободное место и сверяет записи чанков,
  битовые карты и общий счётчик. Проверено на четырёх образах (1024, 1549, 70426, 605459
  блоков) — сходится.

### 2026-11-07, этап 2: транзакция

- `apfs_txn.c`: аллокация/освобождение блоков, битовые карты чанков, внутренний пул с
  кольцом копий битовой карты, новые chunk-info блоки, эфемерные объекты в data ring,
  checkpoint map, коммит суперблока, block zero.
- `apfs-tool txn-mtime`: меняет один inode-рекорд и коммитит. Два прогона подряд (xid 2, 3),
  `apfsck -cw` чистый. Обрыв питания после 0..40 блоков — образ всегда монтируется,
  изменение появляется ровно на блоке коммита.

### 2026-11-07, этап 3: CoW B-дерево, create/unlink

- `apfs_btree_write.c`: запись B-дерева с копированием на запись — вставка, удаление, замена
  одной записи; сплит узла и рост корня; узлы виртуального дерева держат свой oid.
- `apfs_fs_write.c`: создать пустой файл и удалить его: inode-рекорд с xfield имени, запись
  каталога, счётчик детей у родителя, записи в object map, суперблок тома, запись тома в
  omap контейнера.
- `apfs_txn.c`: освобождение блоков отложено до конца транзакции; битовая карта внутреннего
  пула пишется после освобождений.
- Проверено: 12 созданий + 6 удалений на образах 4 МБ и 288 МБ (5 узлов, сплиты),
  после каждого шага `apfsck -cw` чистый.

### 2026-11-07, этап 4: содержимое файлов

- `apfs_append_file`: блоки данных, file-extent запись на каждый непрерывный прогон, запись
  в extent-reference tree (len|kind, sid, refcount 1), размер data stream в xfield инода,
  dstream-id запись. Хвост последнего блока дописывается поверх существующего.
- Суперблок тома: новый корень extent-reference tree (0x90) и block count.
- Проверено: 12 байт, ещё 5120 байт (5132 читаются), образ 288 МБ с 41 файлом, повторное
  дописывание; `apfsck` чистый.

### 2026-11-07: Apple fsck_apfs принимает образы

- macOS 12 отказывала mkapfs-образам: «spaceman struct ranges 0+336 and 0+0 overlap».
  Неверсионированный space manager с массивами на 0x150 Apple не принимает, и tier2-device
  с `sm_addr_offset=0` тоже. `mkapfs.py` переведён на версионированный space manager
  (`sm_flags=1`, `sm_version=1`, `sm_struct_size=2520`, массивы после структуры) и tier2-массив
  поставлен после основного.
- После этого настоящий `fsck_apfs` (macOS 12.7.6) говорит «appears to be OK» (RC=0).
- `mkdir`/`rmdir`: каталог с именем и счётчиком детей, `rmdir` только пустого, счётчики
  num_files/num_directories; `unlink` освобождает блоки файла, extentref и dstream-id.

### 2026-11-07, этап 5: xfield, rename, truncate

- Перепаковка xfield-блока inode (`apfs_fs_write.c`). `inode_xf_list` / `inode_xf_apply`:
  поля читаются списком, нужное заменяется, вставляется по возрастанию типа или удаляется,
  blob пересобирается целиком (дескрипторы, затем данные с padding до 8) через отдельный
  буфер, чтобы не читать то, что уже записано. `inode_set_name` (type 4, с нулём),
  `inode_set_dstream` (type 8, 40 байт; crypto-id и счётчики сохраняются),
  `inode_xfield_del`. Старый `inode_add_xfield` (только дозапись в конец) убран.
- `apfs_rename` (+ `apfs-tool txn-rename`): lookup, проверка свободного имени (EEXIST),
  inode value → новое имя, `parent_id`, `change_time`; удаление старого dir-record,
  вставка нового (file_id, date_added, dtype); nchildren у старого родителя −1, у нового +1
  (если родитель тот же — один раз); каталог внутрь себя — EINVAL. В `<FS_NAME_MAX` —
  ENAMETOOLONG.
- `apfs_truncate` (+ `apfs-tool txn-truncate`): экстенты выше новой длины освобождаются
  (блоки, extentref, file-extent); экстент, в котором кончается файл, обрезается по границе
  блока — file-extent и extentref переписываются на меньшее число блоков; dstream в иноде
  получает size/alloced. Пустой файл: dstream-xfield и dstream-id удаляются (иначе apfsck
  ругается «Data stream: missing reference count»). Рост файла не поддержан (EINVAL).
- `apfs-tool`: `txn-repack IMAGE PATH` (пересобирает запись инода из её же значений, сверяет
  байты и пишет обратно транзакцией — self-check перепаковки), `txn-rename`,
  `txn-truncate`. У `txn-create/unlink/append/mkdir/rmdir` аргумент SECONDS стал
  действительно необязательным (было `argc > 5`, команда без времени не находилась).
- Проверено:
  - `txn-repack` — «bytes identical» на образах 4 МБ и 288 МБ (5 узлов);
  - rename через каталоги, в том же каталоге, переименование каталога (`/dir1 → /dir2`),
    EEXIST на занятое имя, EINVAL на каталог внутрь себя;
  - truncate: 12288 → 5000 → 4096 → 0 → append, обрезка многожэкстентного файла
    (два экстента, между ними чужие блоки), файл-сосед не тронут;
  - обрыв питания на truncate после 0..30 блоков: образ всегда цел, состояние — либо
    старое целиком (12288 и старые байты), либо новое целиком (5000 и верный префикс);
  - `apfsck -cw` чистый после каждого шага; настоящий `fsck_apfs` macOS 12.7.6 на p1…p5,
    q1…q3 и на полной цепочке (mkdir/create/append/rename/rename-dir/truncate/unlink) —
    «appears to be OK», RC=0;
  - `tools/apfs/write-tests.sh` — 96 проверок, 0 провалов (добавлен тест `names` на 18
    проверок: create/append/rename/truncate, все читатели + apfsck, отказы).

### 2026-11-07, этап 6: extended attributes

- `apfs_setxattr` / `apfs_removexattr` (+ `apfs-tool txn-setxattr IMAGE PATH NAME FILE|-`,
  `txn-rmxattr`): ключ `jkey(id, APFS_TYPE_XATTR)` + длина (с нулём) + имя; значение
  `flags | len | data`, флаг `APFS_XATTR_DATA_EMBEDDED`. Встроенные значения только,
  до `APFS_XATTR_MAX_EMBEDDED_SIZE` (3804), больше — E2BIG; потокового xattr нет.
  Вместе с атрибутом двигается `change_time` инода.
- Проверено: поставить/заменить/снять, пустое значение, отсутствующий атрибут — ENOENT,
  3805 байт — E2BIG; переживает rename файла и каталога и truncate; на образе 4 МБ и на
  288 МБ (41 файл, 5 узлов). `apfsck -cw` чистый, `fsck_apfs` macOS 12.7.6 — OK;
  обрыв питания 0..40 блоков на вставке 3804-байтного атрибута — образ цел, атрибут либо
  отсутствует, либо целый. `write-tests.sh` — 108 проверок (тест `xattrs`), 0 провалов.

#### Что пришлось починить в B-дереве (это было сломано и раньше)

- `btp_node_load` проверял `voff + vlen > val_end`, а `voff` в APFS кумулятивный (включает
  свою длину, значения лежат от конца блока назад) — большие значения отвергались как
  «битые». Правильно: `voff > val_end || voff < vlen`.
- `btp_node_pack` при `voff > val_end` уходил в underflow и писал за блок — добавлена проверка.
- Новый корень писался с `vl = 0` у детей (рост корня в тестах раньше не случался) —
  теперь `vl = 8`.
- Сплит был строго на два узла и искал точку деления только вниз от середины: запись
  размером в узел (xattr 3804) не влезала, если по обе стороны от неё есть записи. Теперь
  узел режется на 1..4 части жадной упаковкой, родитель принимает несколько новых записей,
  корень растёт на столько детей, сколько частей.
- При сплите корня первая часть сохраняла oid корня — корень указывал сам на себя
  (бесконечный спуск). Теперь части корня всегда получают новые oid, а старую запись
  корня снимает рост корня.
- Добавлен предел глубины спуска (16) — на всякий случай, вместо переполнения стека.

## Передача следующей сессии (2026-11-07, вечер)

### Где остановились

Работает и проверено (`apfsck` + настоящий `fsck_apfs` macOS 12.7.6 с мака):
space manager (версионированный), транзакции с обрывом питания, CoW B-дерево со сплитами,
create/unlink, append содержимого (экстенты + extentref), mkdir/rmdir, rename, truncate,
перепаковка xfield inode, **встроенные xattr**. B-дерево умеет сплит на 2–3 узла и рост
корня (это понадобилось для 3804-байтных атрибутов и было сломано раньше).

Файлы: `apfs_txn.c`, `apfs_btree_write.c`, `apfs_fs_write.c`, `apfs_txn_priv.h`
(+ правки `apfs_lib.c/.h`, `apfs_format.h`). Сборка: `tools/apfs/build-tool.sh`,
тесты: `tools/apfs/write-tests.sh`, `apfsck` в `xnu-deps/apfsprogs/apfsck/apfsck`.

Инструмент: `apfs-work/apfs-tool` — `txn-create`, `txn-unlink`, `txn-append`, `txn-mkdir`,
`txn-rmdir`, `txn-rename`, `txn-truncate`, `txn-setxattr`, `txn-rmxattr`, `txn-repack`,
`txn-mtime`, `spaceman`, `ls`, `stat`, `cat`.

### Следующий шаг: хуки VFS в ядре

- `apfs_vnops.c` / `apfs_vfsops.c`: `create`, `mkdir`, `unlink`, `rmdir`, `rename`,
  `truncate`/`setattr` (изменение размера), `write` (append-путь через `apfs_append_file`,
  правка внутри блока — `apfs_write.c`), `getattr`/`readdir` уже есть.
- Транзакция на каждую операцию — как в `fs_run`: `apfs_spaceman_open`, `apfs_txn_begin`,
  ... `apfs_txn_commit`; при ошибке `apfs_txn_abort`. Проверять `vol->nx->dev.write`.
- Время брать настоящее (`clock_gettime`/`getnanotime`), а не xid, как в apfs-tool.
- Права/uid/gid в inode — из `kauth_cred`; `nlink` для hardlink пока 1.
- Мьютекс на том: одна транзакция за раз (`apfs_txn` не реентерабелен).

#### Сначала среда: ядро здесь ещё не собиралось

Проверено 2026-11-07 вечером:

- `xnu-src/xnu-8796.121.2` нет; `xnu-src/xnu-8796.121.2-raphael` — пустой `git init`,
  исходников в нём нет.
- `xnu-deps/sdk/iPhoneOS16.5.xnu.sdk` нет; `xnu-deps/setup.sh` не прогонялся
  (есть только `host/bin` с cctools и `xnu-deps/build`).
- clang-18 и theos с `sdks/iPhoneOS16.5.sdk` на месте.

Значит, первая половина сессии — среда, а не код:

1. `xnu-deps/setup.sh --stage1` — Apple open source + `MacOSX13.3.sdk` (интернет, ~1–2 ГБ).
2. `xnu/apply.sh` — `git fetch --depth 1` тега `xnu-8796.121.2` из apple-oss-distributions,
   затем `hakios.patch` и `xnu/files/`. Дерево появится в `xnu-src/xnu-8796.121.2`.
3. `xnu-deps/setup.sh --stage2` — две статические библиотеки против заголовков ядра.
4. `./xnu-build.sh` — **сначала собрать baseline без правок** и убедиться, что сборка
   проходит; только потом трогать `apfs_vnops.c`.

#### Карта: что в VFS уже есть и чего нет

- Таблица операций — `apfs_vnodeop_entries` (`apfs_vnops.c:1237`). Есть: lookup, open,
  close, getattr, read, pagein, readdir, readlink, getxattr, listxattr, inactive, reclaim,
  pathconf, mmap, mnomap, ioctl, fsync (nop). **Нет ни одной пишущей операции** — всё
  остальное уходит в `vn_default_error` (ENOTSUP).
- `apfs_vfsops.c:1168` — `vfs_setflags(mp, MNT_RDONLY | MNT_LOCAL)`, монтирование
  безусловно read-only. `statfs` отдаёт `f_bfree = 0`.
- `struct apfs_devctx` (`apfs_vfsops.c:40`) — только `dc_devvp/devblock/base`; в `dev`
  заполняются `read` и `read_data` (`1078`), **`dev.write` не ставится вообще**. Отсюда
  `fs_run` в `apfs_fs_write.c:1661` всегда возвращает EROFS.
- `struct apfs_node` (`apfs.h:176`) держит `an_ino` и `an_size` как «неизменяемые»;
  getattr/read/pagein берут их оттуда (`apfs_vnops.c:365, 613, 693, 741`). После любой
  записи это надо обновлять, иначе `stat` и чтение будут врать.
- Замок: `apfs_lock(am)` / `apfs_unlock(am)` — один мьютекс на том; все vnop'ы его берут,
  `apfs_lib` умеет отпускать и брать заново (relock). Одна транзакция за раз получается
  сама. `apfs_node_vnode()` берёт замок сам — вызывать его только вне замка.

#### Что писать в каждом хуке

| vnop | чем | тонкости |
|---|---|---|
| CREATE | `apfs_create_file` | после — `apfs_node_vnode`, `cache_enter`, счётчик у родителя |
| MKDIR | `apfs_mkdir` | то же |
| REMOVE | `apfs_unlink` | `cache_purge` старого имени, nchildren-- |
| RMDIR | `apfs_rmdir` | то же |
| RENAME | `apfs_rename` | `cache_purge` старого имени и `cache_enter` нового; `an_parent` у vnode; у каталога — `cache_purge` его самого |
| TRUNCATE | `apfs_truncate` | до: `ubc_msync(vp, 0, old, UBC_PUSHALL)`; после: `ubc_setsize(vp, new)`; `an_size`/`an_ino` |
| SETATTR | size → `apfs_truncate`; mode/uid/gid/flags/времена → **новый** `apfs_inode_setattr` | `apfs_inode_set_mod_time` не годится: требует одноузловых деревьев. Новый API: прочитать value, поправить fixed-часть + change_time, `BTP_REPLACE` через `fs_run` |
| WRITE | в конец — `apfs_append_file`; внутрь/поверх — `apfs_write_in_place` (`apfs_write.c`, структуры не трогает) | длину файла помнит `an_size`; обновлять после |
| SETXATTR / REMOVEXATTR | `apfs_setxattr` / `apfs_removexattr` | уже готово в lib, хуков нет |
| ALLOCATE | ENOTSUP | F_PREALLOCATE никто не ждёт |
| PAGEOUT | в первой версии ENOTSUP | mmap-запись на `/private/var` тогда не заработает; SQLite чаще read/write, но проверить |

#### Устройство и монтирование

- `apfs_dev_write(ctx, off, buf, len)` рядом с `apfs_dev_read`: те же проверки выравнивания,
  запись через buf-слой (`buf_meta_bwrite` / `VNOP_STRATEGY` с `B_WRITE`) — функцию взять
  ту, что есть в заголовках собранного дерева (в `xnu-src` сейчас ничего нет, смотреть
  после `apply.sh`).
- `dev.write = apfs_dev_write` — только если устройство пишется на самом деле: корень
  телефона сейчас ramdisk/ufs, канал на userdata — этап 5. На время разработки —
  boot-arg `apfs_rw=1` и/или файл-образ в qemu.
- `vfs_setflags`: не ставить `MNT_RDONLY`, если `dev.write` есть. `statfs.f_bfree` —
  из `spaceman.free_count * block_size`, `f_ffree` можно оставить 0.
- Отдельного «грязного списка» не нужно: каждая операция — своя транзакция с коммитом.

### Потом: том Data

- Хуки VFS в `apfs_vnops.c` / `apfs_vfsops.c` (create, mkdir, unlink, rmdir, rename,
  truncate, write), writeback, mmap из записываемого файла.
- Канал записи на userdata (`rufs_userdata` по образцу `rufs_darwin`).
- Собрать том Data (`mkapfs.py`, 48 ГиБ), смонтировать в `/private/var`, `var_store` выключить.
- Проверка на телефоне: два прогона, настройки показывают 48 ГиБ.

### Проверка с мака (каждый этап)

```
# из Windows
python C:\Users\kola\AppData\Local\Temp\opencode\mac-check.py <образ>
```

Скрипт кладёт файл на 192.168.0.233 (`andrei`, пароль 6586, временный) и запускает
`fsck_apfs -n`. Ждём «appears to be OK», RC=0.

Образы делать в `~/apfs-test` (в `/tmp` WSL иногда чистится), сборка — из `~/hakios`.

### Грабли, уже пойманные

- `apfsck`/`fsck_apfs` ругались: staging битовой карты ip до применения освобождений;
  новый oid у перезаписанного узла (нужен стабильный); неудалённые старые omap-записи;
  счётчик блоков тома; ключ dstream-id затирал dir-record при unlink; раскладка
  space manager (Apple хочет версионированный).
- xfield: менять поле можно только пересборкой blob целиком (имя — первое, dstream —
  последнее; `xf_used_data` — сумма с padding до 8).
- Пустой файл не должен иметь dstream-xfield без dstream-id: apfsck говорит
  «Data stream: missing reference count».
- Скрипты, записанные с Windows, приходят с CRLF — перед `bash` делать
  `sed -i 's/\r$//'`, иначе аргументы уходят с `\r` и `apfs-tool` не находит команду.

## 2026-11-07, вечер: среда, ядро и первые хуки VFS

### Среда (было пусто — собрали с нуля)

- `xnu-deps/setup.sh --stage1`, `xnu/apply.sh`, `setup.sh --stage2`, `./xnu-build.sh -j24 -k` —
  baseline без правок собрался, ядро `mach.development.bcm2837`.
- Грабли, которые пришлось починить:
  - в WSL glibc 2.35: нет `strlcpy`/`strlcat`, host-утилиты XNU не линковались. Добавлены
    реализации в `xnu-deps/hostinc/hostcompat.h` под `__GLIBC_PREREQ(2, 38)`.
  - после правки `hostcompat.h` старый `BUILD/obj/SETUP/replacecontents.o` не перекомпилировался —
    снести `BUILD/obj/SETUP` (make не знает про force-include).
  - новые файлы APFS не были перечислены в `bsd/conf/files` (`apfs_txn.c`,
    `apfs_btree_write.c`, `apfs_fs_write.c`) — ядро падало на линковке undefined symbols.
  - `apfs_fs_write.c` использовал `strcpy`, а в XNU `string.h` это 3-аргументный макрос —
    заменено на `memcpy(... strlen(...) + 1)`.
- `xnu/export.sh` после сессии: 73 changed, 117 new files.

### Хуки VFS (этап 4) — написано

- `apfs_vfsops.c`: `apfs_dev_write` (буфер + `VNOP_STRATEGY` с `B_WRITE`); перед записью
  `vnode_startwrite`, на ошибке стратегии `vnode_writedone` — без этого первая же запись
  паникует `vnode_writedone: numoutput < 0`. `dev.write` ставится только при boot-arg
  `apfs_rw=1`. rw-монтирование: `writable = (не MNT_RDONLY или MNTK_WANTRDWR) &&
  apfs_write_volume_ok()`; `f_bfree` из space manager; корень всегда read-only
  (Data-том будет монтироваться поверх `/private/var`, а не быть корнем).
- `apfs_vnops.c`: `create`, `mkdir`, `remove`, `rmdir`, `rename`, `setattr` (размер → truncate,
  mode/uid/gid/flags/времена → новый `apfs_inode_setattr`), `write` (append / in-place / гибрид),
  `setxattr`/`removexattr`; `apfs_node_refresh` обновляет `an_ino`/`an_size` после операции.
- **Важно:** lookup для CREATE/RENAME должен возвращать `EJUSTRETURN`, а не `ENOENT`
  (иначе namei не доходит до VNOP_CREATE вообще и `open(O_CREAT)`/`mkdir` умирают на лукапе);
  на ro-томе — `EROFS`.
- `apfs_lib.[ch]`/`apfs_fs_write.c`: owner/group в создании inode, mode у `mkdir`,
  `struct apfs_setattr` + `apfs_inode_setattr`; удаление `com.apple.decmpfs` чистит
  `APFS_INODE_HAS_UNCOMPRESSED_SIZE` и `UF_COMPRESSED` (иначе apfsck: «Compressed file:
  missing decmpfs xattr»).
- `virtio_blk`: запись под boot-arg `virtio_blk_write=1` (по умолчанию read-only), `T_OUT`,
  `d_write`, правильные descriptor flags.
- `apfs-tool`: `txn-create IMAGE DIR NAME [SECONDS [MODE]]`.

### Стенд QEMU (готов, лежит вне репозитория)

- `qemu-system-arm` 6.2 + AAVMF: `QEMU_EFI=/usr/share/AAVMF/AAVMF_CODE.fd`,
  `QEMU_VARS=/usr/share/AAVMF/AAVMF_VARS.fd`; `-cpu max` (cortex-a76 в 6.2 нет).
- Тестовый APFS — virtio-диск (`-drive file=rwtest.apfs,format=raw,if=none,id=d` +
  `-device virtio-blk-device,drive=d`), boot-args `apfs_rw=1 virtio_blk=1 virtio_blk_write=1`.
- root-образ собран `mkapfs.py` из минимального дерева: `sbin/init`, `etc/rc`, `etc/var.skel`,
  `LaunchDaemons/com.hakios.demo.plist`, `bin/apfs_rw_test` (mode 0755 — у `txn-create` теперь
  есть MODE); `/etc/rc.jobs` запускает `/redwood/bin/apfs_rw_test`.
- `userland/src/apfs_rw_test.c` — тест из user space: mount rw, чтение, create+write+readback,
  mkdir, rename, truncate, setxattr, unlink, rmdir.
- Грабли стенда: `timeout` в `qemu/run.sh` шлёт только TERM, QEMU его игнорирует и висит,
  держа порт 4444 — убивать `kill -9` по pid.

### Где остановились

- Хост-тесты: `write-tests.sh` — 109 проверок, 0 провалов (после всех правок lib).
- В QEMU: том монтируется rw, чтение работает, первый `create` доходит до `apfs_create_file`
  и **коммитит транзакцию**: образ на хосте после прогона имеет xid 2, 3 файла, `apfsck`
  чистый. То есть запись ядром реально идёт и Apple-совместима.
- Но затем следующий `apfs_spaceman_open` в госте читает блоки checkpoint ring
  («block 3/4» или «5/6: bad checksum»), после чего «the checkpoint map names no space manager».
  Похоже на устаревший кэш блоков `apfs_lib` после записи (host-tool каждый раз открывает образ
  заново, поэтому не проявляется): проверить, обновляет ли `apfs_txn_commit` `nx->xid`,
  `nx->sb`, `xp_desc_index`, и инвалидируются ли кэшированные блоки, которые транзакция
  переписала по тем же адресам.
- Отдельно: в одном прогоне `create` вообще не дошёл до хука (VFS вернул EROFS), в другом —
  дошёл. Подозрение на флаги маунта (`MNT_RDONLY` в `v_mount`) или гонку; добавить лог
  итоговых флагов маунта и проверить стабильность.

### Следующей сессии

1. Починить инвалидацию кэша/обновление состояния nx после коммита; добиться, чтобы
   `apfs_rw_test` прошёл все шаги с 0 провалов.
2. Проверить изменённый образ: `apfsck -cw` и настоящий `fsck_apfs` с мака.
3. Этап 5: канал записи на userdata (`rufs_userdata` по образцу `rufs_darwin`,
   SCSI WRITE(10) + SYNCHRONIZE CACHE, границы раздела, разрешение в лоадере).
4. Этап 6: том Data 48 ГиБ, монтирование в `/private/var`, `var_store` выключить.

### 2026-11-07, продолжение: где именно врёт запись

- `open(2)` на запись чинился: `apfs_vnop_open` отдавал EROFS на `FWRITE|O_TRUNC|O_CREAT`
  всегда — теперь только на ro-томе. После этого `open(O_CREAT)` проходит.
- Кэш после коммита: добавлен `apfs_nx_cache_drop()`, `apfs_txn_commit` сбрасывает из кэша
  каждый записанный блок. На баг это не повлияло, но правка правильная.
- Сырой тест устройства в `apfs_rw_test` (pwrite+pread 4 КиБ по смещению 1000*4096 через
  `/dev/rvblk0`): **ok, содержимое совпадает** — virtio-запись и чтение сами по себе верные.
- Остаётся: после первого успешного коммита следующий `apfs_spaceman_open` читает блок
  (105, в других прогонах 3/4/5/6) и получает «bad checksum», дальше EIO. Проверка образа
  на хосте: этот блок **пустой (нули)**, т.е. на него кто-то ссылается, но его никто не писал.
  `txn_write_all` пишет каждый блок и сразу читает обратно с `memcmp` — сообщения о
  расхождении нет, значит блок в транзакцию не попадал вообще.
- Версия: расходятся адреса эфемерных объектов/checkpoint-кольца между состоянием в памяти
  (`nx->xp_desc_*`, `xp_data_*`, `nx->xid`) и тем, что реально записано. Смотреть
  `txn_ephemerals()`/`apfs_txn_commit()`: какие `at` получаются для spaceman и прочих map-
  записей, совпадают ли они с `nx->xp_data_base`; и не остаётся ли в `nx->sb` старый
  `xp_data_base`/`desc_base`. На хосте каждый процесс открывает образ заново, поэтому там
  это не воспроизводится.
- После прогона `apfsck` на образе: «Extent reference tree node: wrong object subtype» —
  то же самое: ссылка на узел, которого нет.
- Замечание по именам: `redwood` — кодовое имя **POCO X5 Pro**, это конфиг QEMU-стенда
  (`BCM2837`). Телефон пользователя — **raphael** (Mi 9T Pro), ветка `raphael-bringup`,
  и для него сборка идёт с `XNU_SRC=xnu-src/xnu-8796.121.2-raphael` (сейчас там пустой
  `git init` — дерево под raphael надо будет применить отдельно). Наш APFS-код от машины
  не зависит, но финальная сборка для телефона — из raphael-дерева.

### 2026-11-07, ночь: корень бага найден (B-дерево путает oid и paddr)

Логи с тегами (`alloc`/`emit`/`free`/`read_block`) дали точную картину. В журнале xid3:

```
fs_open: fs root 95 (oid 1029) omap 97 extref 67
btp_open 0xe root 95; ... btp_open 0xb root 97; btp_open 0xf root 67
read 95 -> free 95 -> alloc 62, emit 62 oid 1029
read 67 -> free 67 -> alloc 63, emit 63 **oid 63**     <-- физический oid у узла
                                виртуального дерева
read 62 -> free 62 -> alloc 64, emit 64 oid 1029
```

- В виртуальном дереве (fs tree, flags 0x40) узел описывается **oid** (>=1024, резолвится
  через omap), а у нас ребёнок получает `oid 63` — то есть в записи ребра лежит **paddr**,
  а не oid. Из-за этого:
  - `btp_update` идёт в «ребёнка» по адресу 67 — а это **корень extentref-дерева**;
  - транзакция освобождает 67 (`free 67`), хотя на него ссылается суперблок тома
    (`apfs_extentref_tree_oid = 0x90`);
  - на следующем шаге аллокатор выдаёт 67 под узел fs-дерева, и через пару коммитов
    `free 67` повторяется → «block 67 is given back but was free already» → EIO;
  - когда транзакция падает, `new nx omap object` (блок 105) уже выделен, но не записан —
    отсюда `block 105: bad checksum` при следующем `spaceman_open`.
- Гипотеза Gemini про `0x80 vs 0x90` для `vol->omap_addr` **не подтвердилась**: лог
  `commit: old omap …` печатает 64 → 98 → 94 → 97 (правильные адреса `omap_phys_t` тома),
  ни разу не 67. 67 освобождает B-дерево, а не `fs_commit`.
- Где искать: `btp_update_node()` — при спуске ребёнок берётся как
  `t->physical ? child_oid : resolve(child_oid)`, а при записи родителя в ребро кладётся
  `co.oid`/`co.part_oid[]`; в одной из ветвей (сплит части / рост корня / повторный
  `btp_update` того же узла) в ребро попадает адрес вместо oid. Проверять:
  `btp_emit` + место, где родитель заполняет `n.e[pos].v` (`b_put64(n.e[pos].v, co.oid)` и
  `co.part_oid[j]`), и `btp_new_oid()` при `physical=false`.
- Типовой аудит на следующую сессию: после `op` и перед `txn_write_all` обойти новое дерево
  и проверить, что **каждое значение ребра виртуального дерева есть oid (>=1024), а не
  адрес блока**; и что ни один блок, который транзакция освобождает, не равен
  `vol->extentref_oid`, `vol->omap_addr`, `vol->sb_addr`, `nx->omap_addr`.

#### Ответ Gemini по утечке paddr в ребро (сохранить для следующей сессии)

Четыре места, где в виртуальном дереве `co.oid` может стать адресом:

1. При рекурсивном спуске в `btp_update_node` передан `child_addr` вместо `child_oid`,
   и он же ушёл в `btp_emit` как `oid` (тогда `noid = oid` и в ребро/omap пишется адрес).
2. Рост корня: новый корень должен забрать `t->root_oid`, а части — получить **новые** oid
   (`btp_new_oid`), иначе левое ребро = 1029 (петля на себя) или = paddr старого корня.
3. `btp_emit` пишет в заголовок блока (`obj_phys_t.o_oid`) `at` вместо `noid` — тогда
   со следующей транзакции узел «считает» своим oid физический адрес.
4. `noid = oid ? oid : btp_new_oid(t, at)` при `t->physical` корректен только если для
   физического дерева `oid` всегда 0; если передать старый адрес, новый узел останется
   со старым адресом.

Проверка одной строкой в `btp_emit` / перед `b_put64(n.e[pos].v, co.oid)`:

```c
if (!t->physical && co.oid < 0x400)
        panic("VIRT BTREE PADDR LEAK: co.oid=%llu in_oid=%llu at=%llu level=%u",
              co.oid, oid, at, n->level);
```

Инвариант: виртуальное дерево — в ребре только oid (>= 0x400), заголовок блока —
`o_oid = noid`; физическое — в ребре и заголовке адрес блока. Проверить в нашем коде:
вызов `btp_update_node(t, child_addr, child_oid, ...)`, ветку `keep_oid` при сплите,
`noid` в `btp_emit` и запись `o_oid` в `btp_node_pack`.

## 2026-11-08: стенд QEMU зелёный, запись ядром проверена

### Причина всех «bad checksum / block 105»

`fs_commit` после успешного коммита обновлял `vol->tree.root`, `vol->omap.tree.root`,
`vol->omap_addr`, `vol->sb_addr` — но **не `vol->extentref_oid`**. Новый корень
extentref-дерева писался в суперблок тома (поле `0x90`), а в памяти оставался старый.
Следующая транзакция открывала extentref по уже освобождённому блоку, освобождала его
второй раз («block 67 is given back but was free already» → EIO), аллокатор успевал отдать
этот блок другому дереву (потом `apfsck`: «extentref node: wrong object subtype»).
А в упавшей транзакции блок `new nx omap object` (105) уже был выделен, но не записан —
отсюда `block 105: bad checksum` при следующем `apfs_spaceman_open`.

Правка: после коммита `vol->extentref_oid = extref_root` (когда дерево есть).

### Ещё одна правка: unlink и атрибуты

`apfs_unlink` удалял inode и dir-record, но не xattr-записи — `apfsck` говорил
«Catalog: xattr record with no inode». Теперь `fs_remove_file` перечисляет атрибуты
(`apfs_xattr_list`) и удаляет каждую запись.

### Проверено

- `tools/apfs/write-tests.sh` — **109 проверок, 0 провалов** (после всех правок).
- QEMU: `apfs_rw_test` — **RESULT: 0 failure(s)**: mount rw, create, write+readback (100 и
  4096 байт), mkdir, create внутри, rename, truncate, setxattr, unlink, rmdir.
- Образ после прогона (xid 17, записан ядром через virtio): `apfsck -w` — чисто;
  настоящий `fsck_apfs` macOS 12.7.6 — «The container appears to be OK», RC=0, включая
  extentref и «Verifying allocated space».
- Отладочные логи (`alloc/emit/free/read_block/spaceman_find/omap_find/btp_open/fs_open/
  commit …`) убраны; оставлены только сообщения об ошибках и проверка
  «виртуальное дерево не должно получить адрес в ребро» (`PADDR LEAK`).

### Что дальше

1. Собрать ядро под **raphael** (`xnu-src/xnu-8796.121.2-raphael`, ветка `raphael-bringup`)
   и прогнать `apfs_rw_test` на телефоне (том-образ на RAM-диске или rw-монтирование
   существующего тома), затем этап 5.
2. Этап 5: канал записи на userdata (`rufs_userdata` по образцу `rufs_darwin`, SCSI
   WRITE(10) + SYNCHRONIZE CACHE, границы раздела, разрешение записи в лоадере).
3. Этап 6: том Data 48 ГиБ, монтирование в `/private/var`, `var_store` выключить.

- 2026-11-08: второй прогон `apfs_rw_test` на том же образе (xid 17 → 33, том записан
  прошлым прогоном ядра) — снова `RESULT: 0 failure(s)`; `apfsck` чистый. `fsck_apfs`
  с мака для второго образа не снят: SSH на 192.168.0.233 перестал отвечать (пинг есть,
  22 — таймаут, видимо выключен Remote Login). Для первого образа (xid 17) проверка была.

### 2026-11-08: сборка под raphael и прогон с этим ядром

- `xnu-src/xnu-8796.121.2-raphael` пересоздан из `xnu/apply.sh` (тег `xnu-8796.121.2`
  + 73 изменённых файла + 117 новых; APFS-файлы на месте).
- `XNU_SRC=.../xnu-8796.121.2-raphael ./xnu-build.sh -j24 -k` — `built kernel`, exit 0,
  `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837` (15 419 656 байт).
- QEMU-стенд с этим ядром: `apfs_rw_test` — `RESULT: 0 failure(s)`; `apfsck -cw` по
  `rwtest.apfs` чистый; `fsck_apfs` macOS 12.7.6 с мака — «The container appears to be
  OK», RC=0 (xid 49).
- Телефон в fastboot (Windows, `6b9bc191`) виден, но в WSL USB не проброшен, поэтому
  прогон на телефоне не делали. Этап 5 в коде уже есть (`rufs_userdata`/`BLOCK_RAPHAEL_*`
  в `hakios.ufs.c` и `loader.c`, коммит 79f4333), но на телефоне не проверен.
  Этап 6 (том Data 48 ГиБ, `/private/var`, `var_store=0`) не начат.
### 2026-11-08, вечер: этап 5 закрыт на телефоне, этап 6 начат

**USB без usbipd.** Windows-`fastboot.exe` (`Documents\hackios\tools\fastboot.exe`) говорит
и с загрузчиком, и с самим `xnuloader` (у него те же VID/PID 18D1:D00D, драйвер
«Android Bootloader Device Interface» подходит). Если после `fastboot boot` телефон
отвечает `product: raphael` — нужен `fastboot reboot bootloader` и повторный `boot`.
Весь цикл (boot UEFI → oem ramdisk → stage payload → oem run → get_staged → oem read-part)
идёт из Windows, без проброса USB в WSL.

**Этап 5: запись в userdata с телефона.** `apfs_rw_test` научился:

- брать устройство, файл-образ тома и смещение в МиБ: `apfs_rw_test DEV SEED MB [seed]`;
- захватывать стор (`kern.hakios.var_busy=1`) перед сырой записью через `/dev/rufs_NAME` —
  без него `ufs_cwrite` отдаёт EPERM (это и была первая ошибка на телефоне);
- монтировать том не с начала раздела: `mount("apfs", dir, 0, "/dev/ufs_userdata,b=102400")`
  (новый разъём `b=MB` в `apfs_mount`, байтовое смещение уходит в `dc_base`);
- не перезаписывать образ, если на смещении уже есть NXSB (второй прогон проверяет
  сохранность тома, а не пересев).

Прогон на телефоне (root из flash `root_apfs=ufs_userdata`, `ufs=2 ufs_native=1 ufs_write=1`):
контейнер 4 МиБ записан в userdata на 100 ГиБ через `/dev/rufs_userdata`, том смонтирован
rw (`/dev/ufs_userdata,b=102400`), все операции — `RESULT: 0 failure(s)`; образ снят обратно
через `oem read-part userdata 0x1900000 0x400` + `get_staged`, `apfsck -cw` чистый.
`s.0` настоящего `fsck_apfs` пока нет: мак спал, SSH не отвечал.

**Два настоящих бага ядра, найденных на этапе 6** (воспроизводятся и на хосте):

1. `fs_add_file`: `val[128]` под inode-record мало для имён длиннее ~16 символов —
   `memset` паддинга имени вылетал за буфер: на хосте `*** buffer overflow detected ***`,
   в ядре паника `__memset_chk object size check failed (28 < 32)`.
   Буфер стал `92 + 8 + round8(FS_NAME_MAX)`.
2. `apfs_inode_value_get`: искал запись только в одном листе. После сплита inode-record
   мог оказаться первым ключом листа, а `seek_to` приходит в него из предыдущего листа —
   запись «не находилась». Отсюда `chmod`/`chown` на старых inode'ах (созданных в
   прошлую загрузку) давали ENOENT, а на только что созданных работали. Теперь читает
   через `apfs_seek` + `apfs_cursor_next`, как `apfs_inode_get`.

**Этап 6: том Data 48 ГиБ.** Готово:

- `mkapfs.py` собирает 48 ГиБ контейнер разряжённым файлом (144 КБ реальных данных,
  49 ГБ видимых); `tools/apfs/seed-chunks.py` превращает его в список `HCK1`
  (вид записи 0/1, смещение, длина) — 149 КБ вместо 105 МБ, влезает в payload.
- `apfs_rw_test ... seed` пишет контейнер в userdata с 8 ГиБ за один прогон (105 МБ записи,
  ~1 минута); второй прогон видит NXSB и не пересевает.
- `init.c`: boot-arg `var_apfs=DEV[,b=MB]` — `/private/var` монтируется с APFS-тома вместо
  ramfs (с откатом на ramfs, если не смонтировалось); каталоги скелета терпят EEXIST и не
  обрывают список на первой ошибке (иначе на живом томе во второй загрузке создавалось
  21 каталог из 111).
- И в QEMU, и на телефоне том монтируется: `apfs: volume 0 mounted read-write, 49146 MB free`,
  `/private/var: the APFS volume /dev/ufs_userdata,b=8192`, 111 каталогов, `chmod` больше
  не падает.

**Где застряли:** после `exec /sbin/launchd.ios` ignition отрабатывает, а затем процесс
перезапускается, и dyld пишет `Library not loaded: /usr/lib/libSystem.B.dylib
(no such file, no dyld cache)`. В QEMU это паника `initproc failed to start`, на телефоне
ядро вернулось раньше времени (похоже, `init` дошёл до `reboot(RB_HALT)`).
С ramfs на `/private/var` тот же образ грузится нормально — то есть дело не в APFS-записи,
а в чём-то, что меняется, когда `/private/var` становится отдельным томом.
Кэш dyld лежит в `/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld`
(пути ищутся в `vm_unix.c`, `scdir_path`). Дальше: лог целиком (USB-передача `get_staged`
обрывается на 32–45 КБ, нужен `-v` на экран или разбиение выборки), проверить, не теряется
ли путь к кэшу при отдельном монтировании `/private/var`, и не нужен ли
`kern.hakios_switch_root`/preboot-путь, как в flash-загрузке.### 2026-11-08, ночь: что ещё выяснилось про этап 6

**Чтение тома с флеша без Android.** `fastboot oem read-part userdata <lba> <count>` +
`get_staged` по 4 МБ (старшие читаются с `0x200000` = 8 ГиБ) — рабочий способ снять том
обратно. Сценарий: `%TEMP%\opencode\dump48.ps1 -Chunks 32` даёт первые 128 МБ контейнера
(вся метадата там), потом `dd` в разряжённый файл нужной длины и `apfs-tool`.

**Состояние тома Data после загрузки с ним на `/private/var`:**

- `xid 967`, 304 файла, 114 каталогов, 533 блока — init и launchd реально писали на том
  (скелет 111 каталогов + то, что успел launchd).
- `apfsck -cw`: `B-tree node: block 0x6fd6 is not sane.` Блок 0x6fd6 = 28630: контрольная
  сумма верная, oid 0x4bb, xid 967, `btn_flags = 0x0002` (лист, не корень), `btn_nkeys = 0`,
  тело пустое. То есть наш btree-writer оставил в дереве **пустой лист-ребёнок**; apfsck
  требует такие удалять (`node_is_valid`: «Empty children should just be deleted»).
  Это баг в `apfs_btree_write.c` (удаление/слияние), не порча записи: сумма сходится,
  значит блок записан ровно таким, каким его собрал код.

**Почему boot с томом на `/private/var` не доживает до системы.** После
`exec /sbin/launchd.ios` ignition отрабатывает, дальше dyld: `Library not loaded:
/usr/lib/libSystem.B.dylib (no such file, no dyld cache)`; в QEMU это паника
`initproc failed to start`, на телефоне ядро возвращается в loader. С ramfs на
`/private/var` тот же образ грузится. Кэш dyld лежит в
`/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld` (в образе нет
`/System/Library/Caches`), пути поиска — `bsd/vm/vm_unix.c`, `scdir_path`; сам поиск делает
dyld в юзерспейсе. Проверить дальше: не мешает ли отдельный том на `/private/var` тому,
как dyld находит кэш (например через `/tmp` → `private/var/tmp`), и не нужен ли для
`/private/var` вариант «том внутри контейнера корня», как у настоящего iOS, а не отдельный
контейнер на 8 ГиБ.

**Лог через USB обрывается.** `compose_logs` кладёт сперва лог лоадера (`log_buf`, 32 КБ,
он же ограничивает serial-лог ядра), потом лог ядра; `get_staged` стабильно приносит
32–52 КБ, дальше передача рвётся. Хвост прогона так не увидеть: смотреть экран (`-v`)
или снимать состояние с самого тома через `read-part`.

**Что делать дальше (следующая сессия):**

1. `apfs_btree_write.c`: удалять пустые листья-дети (и/или не порождать их при удалении),
   тест на хосте (`write-tests.py`) + прогон на телефоне, затем `apfsck -cw` по снятому
   с флеша тому (скрипт `dump48.ps1`).
2. Разобраться с dyld/`/private/var`: попробовать смонтировать Data не отдельным
   контейнером на 8 ГиБ, а вторым томом в контейнере, где лежит system.apfs (как на
   настоящем iPhone: Data — том того же контейнера); либо проверить гипотезу про `/tmp`.
3. `fsck_apfs` с мака по снятому тому (файл 48 ГиБ разряжённый — по SSH тянет 48 ГБ,
   надо резать образ до первых 128 МБ + хвост или проверять на маке локально).### 2026-11-08, ночь-2: пустые листья и oid'ы узлов закрыты; том Data монтируется, но read-only

**Что починено в btree-writer'е и вокруг него (всё проверено на хосте и в QEMU).**

1. **Пустой лист-ребёнок.** `btp_update_node` (`apfs_btree_write.c`) теперь не пишет узел,
   у которого не осталось ни одной записи: блок возвращается аллокатору, а родитель выбрасывает
   запись, которая на него указывала (`out.oid == 0` — «этого ребёнка больше нет»). Вверх это
   идёт каскадом: если от этого опустел и промежуточный узел, он тоже удаляется. Корень —
   исключение: пустое дерево пишется пустым листом (`level = 0`), а не пустым внутренним узлом.
   В omap для удалённого узла теперь уходит только *удаление* записи: `btp_change_gone()` кладёт
   изменение с `oid = 0`, `fs_publish()` по нему удаляет `(old_oid, old_xid)` и ничего не вставляет.
   Попутно это вылечило вторую болезнь: у пустого листа оставался старый ключ в родителе, и
   `unlink` соседних файлов падал с ENOENT (на 40 файлах — 3 отказа, на 120 — два десятка).

2. **Перепутанные oid'ы: дерево ссылалось само на себя.** `mkapfs.py` писал в суперблок тома
   `next_obj_id` из счётчика томов (16+), а oid'ы узлов дерева выдаёт контейнерный счётчик
   (1024+). Ядро, увидев счётчик ниже 1024, поднимало его до `APFS_OID_RESERVED_COUNT`, и на
   шестом сплите новый узел получал oid, равный oid'у корня: apfsck —
   `Object map record: oid used twice in latest checkpoint`, а корень указывал сам на себя.
   Теперь `mkapfs` ставит `next_obj_id = max(v.next_oid, self.next_oid)`; в ядре страховка:
   `btp_open` поднимает счётчик выше `root_oid`, `btp_new_oid` не выдаёт `root_oid`.

3. **Разъём `b=MB` у `apfs_mount`** вернулся в dev-дерево (он жил только в `xnu/files` и был
   потерян при `xnu/export.sh` из dev-дерева). Синтаксис: `DEVICE[,c=N][,v=M][,b=MB]`;
   `base` уходит в `apfs_mount_common` и садится в `dc_base`. Без него `mount(... , ",b=8192")`
   отдавал EINVAL ещё в разборе аргумента.

**Проверки.** `tools/apfs/write-tests.sh`: 116 проверок, 0 падений; новый тест `empty`
(48 файлов в `/sub` — сплит дерева, потом 48 `unlink` по одной транзакции): apfsck чист,
пустых детей нет, утёкших записей omap нет, каталог пуст. QEMU (`qemu-rw.sh`):
`apfs_rw_test` — `RESULT: 0 failure(s)`, `apfsck -cw` по образу чистый.

**Телефон.** Том Data снова монтируется на `/private/var` — но **read-only**. Что выяснено
(всё видно в логе `init`, см. ниже, как ядро теперь говорит о себе):

- `mount /dev/ufs_userdata,b=8192`: `writable 1`, флаги после монтирования `0x4001000`
  (`MNT_RDONLY` снят), `kern.apfs_mounts` показывает `/dev/ufs_userdata on /private/var:
  flags 0x4001000` — то есть маунт на месте и rw, объём проходит `apfs_write_volume_ok`.
- Тем не менее `open(O_CREAT)`/`mkdir` на `/private/var` дают `EROFS`, и **vnop'ы APFS при
  этом не вызываются вообще** (breadcrumb'и в `apfs_vnop_create`/`mkdir`/`lookup` не сработали,
  счётчик `checks` в `kern.apfs_stats` растёт только на lookup'ах). Значит отказывает VFS —
  `vnode_authorize()` (`vfs_subr.c`, «Check for read-only filesystems») видит у каталога
  **другой** маунт, не наш.
- Второй `mount` того же устройства на тот же `/private/var` проходит (XNU разрешает
  стопку маунтов) — то есть маунт-то есть, но обход пути в него не заходит.
  Главный подозреваемый: `/private/var` на подготовленном flash-корне — *синтетический*
  каталог APFS (или иначе не тот vnode, на который встал маунт), поэтому обход получает
  каждый раз новый vnode без маунта. Проверять: сделать на образе корня настоящий каталог
  `/private/var`; либо монтировать Data не на `/private/var`, а на свежий каталог,
  созданный на уже смонтированном томе.

**Как теперь смотреть, что говорит ядро.** Ядро на телефоне печатает в UART, в USB-лог
(`get_staged`) его вывод не попадает (в лог идут только записи `init` в `/dev/console`).
Поэтому добавлено:

- `kern.apfs_stats`: первая строка — итог последнего маунта (`mount volume "data" base 8192
  rw 1 write 1 flags ... writable 1: -`), вторая — `checks N writable questions`;
- `kern.apfs_mounts`: все APFS-маунты, как их видит VFS (`f_mntfromname on f_mntonname:
  flags ... kern ... role ...`);
- `init` после монтирования `/private/var` печатает первую строку `kern.apfs_stats`,
  пробует записать `/private/var/.rwprobe` и печатает результат.

**Логи прогонов** (в `%TEMP%\opencode`): `p7-bootH.log` — маунт, `p7-bootK.log` — двойной маунт
и список маунтов; `p7-live.log` — первый сид 48 ГиБ (110 записей, 105 МБ) прошёл.

**Что делать дальше (по порядку):**

1. Починить маунт: либо настоящий `/private/var` на образе корня (тогда маунт встанет на
   обычный vnode), либо монтировать Data на каталог, созданный на самом томе
   (`mount` в `/mnt`… нельзя — на RO-корне нет каталога; вариант: ramfs `/mnt`, `mkdir
   /mnt/var`, `mount apfs ... /mnt/var`, потом bind/synthetic на `/private/var`).
2. Когда `mkdir` на томе пойдёт — прогнать загрузку, снять первые 128 МБ (`dump48.ps1
   -Chunks 32`, `check48b.sh`) и получить `apfsck -cw` чистый: это и есть проверка
   пустых листьев на реальной работе init/launchd.
3. Дальше — dyld/`/private/preboot`: после того как том станет rw, повторить замер
   `Library not loaded: /usr/lib/libSystem.B.dylib`.
**Найдено 2026-11-09 (после разбора с Gemini и трассировки).** Обход пути в маунт
**попадает**: `apfs_vnop_lookup` для `open(".rwprobe2", O_CREAT)` видит
`dvp mount /private/var flags 0x4001000, am mount /private/var flags 0x4001000, obj_id 2`,
то есть dvp — корень нашего тома, маунт rw. Коллизии `obj_id == 2` между томами нет (кэш
vnod'ов внутри `apfs_mount`, `vfsp.vnfs_mp = am->am_mp`, `vnfs_markroot` по `am_root_id`).
Настоящая причина — ранний выход в `apfs_vnop_lookup`:

    error = cache_lookup(dvp, ap->a_vpp, cnp);
    ...
    if (error == ENOENT) {              /* «имени точно нет» из негативного кэша имён */
        return ENOENT;                  /* ветка CREATE/RENAME ниже не выполняется */
    }

Обёртка вокруг lookup показала: для CREATE он возвращает `-2` (ENOENT) вместо `EJUSTRETURN`,
и VFS на CREATE дальше отказывает EROFS. Правка: в этой ветке — то же решение, что и в
обычной (`EJUSTRETURN`, если том пишется, иначе EROFS). Проверяется на телефоне
(payload `phone7b-payload.bin`, ядро raphael от 2026-11-09 00:3x).
### 2026-11-09, утро: том Data пишется на телефоне, apfsck чистый

**Причина `EROFS` на записи найдена, и это не VFS.** Цепочка хлебных крошек (см. ниже, как
читать) показала: lookup отдаёт `EJUSTRETURN` (`-2`, это не ENOENT!), авторизация 0,
`vn_create` → `apfs_vnop_create` → `fs_run`: гейт тома в порядке
(`write 1 sealed 0 meta 0 snapshots 0 role 0x0 extref 1`), `spaceman 0`, `txn 1`, `open 0`,
`op 0`, и **`commit -> 30` (EROFS)**.

Причина: коммит APFS пишет блоки через block device (`VNOP_STRATEGY` на `/dev/ufs_userdata`),
а `ufs_strategy()` в `bsd/dev/hakios.ufs.c` на любую запись отвечает `EROFS` («the block
device never writes»). На QEMU это не видно (virtio-blk пишет, когда `virtio_blk_write=1`),
на телефоне — глухо. Этот путь (блок-запись для раздела с data volume) был в прошлой сессии
и жил только в `xnu/files` — `xnu/export.sh` из dev-дерева его снёс, как и разъём `b=MB`.

**Правка:** в `ufs_strategy()` для `ufs_write_part` (и только при `ufs_write=1`) запись идёт
через существующий `ufs_write()` — он держит окно после `UFS_WRITE_FIRST`, проверяет
принадлежность раздела и границы, а лоадер читает каждый блок обратно перед ответом; сразу
после блока лист контроллера возвращается (`ufs_write_restore()`), чтобы чтения шли как
раньше. Все остальные разделы по-прежнему `EROFS`.

**Результат на телефоне** (`p7-bootW.log`):

```
a write on /private/var: ok
a write on .rwprobe2 there: ok
fs_run: ... commit -> 0
111 directories, 10 files
[ ok ] create the directories of the data volume
```

Снятый с флеша том (`dump48.ps1 -Chunks 32` + `check48b.sh`):

```
xid 1271, files 5, dirs 113, blocks 57, tree: 242 records in 50 nodes
apfsck -cw: rc=0
```

То есть **фикс пустых листьев проверен на живой записи**: дерево доросло до 50 узлов (сплиты
были), пустых детей нет, утёкших записей omap нет.

**Что ещё видно в том же логе.** Часть самопроверок `init` на томе падает: `overwrite in the
middle`, `append`, `truncate`, `rename`, `hard link`, `symbolic link`, `shared mapping`; при
этом `create + write 100 KB + read back` и `300 files in one directory` проходят. Это
следующая работа по записи (и, возможно, часть истории с dyld: launchd пишет в
`/private/var/...`). Также `[FAIL] graft the package manager's prefix` — `/jb/prep.sh:
Operation not supported` на настоящем томе.

**Отладка ядра на телефоне.** Вывод ядра уходит в UART и в USB-лог (`get_staged`) не попадает —
в лог попадают только записи `init` в `/dev/console`. Поэтому добавлено:

- `kern.apfs_stats`: первая строка — итог последнего монтирования, вторая — счётчик проверок
  записи, дальше — счётчики APFS;
- `kern.apfs_mounts`: все маунты, как их видит VFS;
- `apfs_why_append()` (в `apfs_vfsops.c`) — одна строка-«хлебная крошка» через `vsnprintf`,
  которую `init` печатает: `lookup ... -> -2; do_create: authorize -> 0; vn_create: attributes
  -> 0; create "...": entered; fs_run: ...; commit -> 30`. Эти крошки сейчас стоят в
  `apfs_vnop_create`, `fs_run`, `vn_create`/`vn_open_auth_do_create` и в трёх местах VFS, где
  возвращается `EROFS`. Их надо будет убрать (или оставить за boot-аргументом).

**Хвост лога.** `get_staged` стабильно отдаёт первые 32 КБ (ядро вернуло 177 КБ:
`loader_return.c` держит 256 КБ головы + 1 МБ кольца). Для разбора dyld нужен именно хвост:
добавить `kern.hakios_klog_reset` (обнулить `loader_log_len`) и звать его из `init` перед
`run_rc("/etc/rc")` — тогда после сброса в логе останутся только ignition, launchd и паника.
**2026-11-09, 01:20: гонка в записи (первый же коммит после монтирования).** На прогоне с
`klog_reset` система упала сразу после `apfs: mount volume "data" ... writable 1`, на первой же
записи (`a write on /private/var` даже не дошло до печати). Паника:

```
panic(cpu 0): Mutex 0xfffffff78d0bc010 is unexpectedly not owned by thread
Panicked task: pid 1 init
  lr: ___lck_mtx_owned_panic
  lr: _lck_mtx_assert
  lr: _nx_block_unlocked + 0xb4        <- nx_unlock(nx) в apfs_lib.c
  lr: _panic_trap_to_debugger
```

`nx_block_unlocked` снимает блокировку тома (`nx_unlock` → `apfs_lib_unlock` →
`lck_mtx_unlock(&am_lock)`), и ассерт говорит, что этот поток её не держит. То есть в новом
пути блок-записи (`ufs_strategy` → `ufs_write` → лоадер) блокировка тома снимается дважды или
её снимает не тот поток. На прогоне без `klog_reset` (`p7-bootW.log`) тот же путь отработал:
create, commit 0, 111 каталогов, apfsck чист — значит это **гонка**, а не логическая ошибка
маршрута. Смотреть: `apfs_lib_unlock`/`nx_unlock` (кто и когда отпускает `am_lock` вокруг
`dev.write`), и не спит ли `ufs_write` слишком долго, держа чужие блокировки (4 фоновых потока
APFS в это время читают).

**Хвост лога.** `kern.hakios.klog_reset` (sysctl, `osfmk/arm64/loader_return.c`:
`loader_log_reset()` + вызов из `init` перед `run_rc`) добавлен, вызов проходит без ошибки, но
журнал всё равно начинается с ранней загрузки — значит `loader_log_len`/`CTX_LOG_LEN`
обновляются не так, как ожидалось (проверить в `loader_return.c`: кто и когда пишет слот).
Тем не менее за один прогон `get_staged` отдал 52 КБ и в них попал весь хвост вплоть до паники —
иногда передача проходит дальше 32 КБ, пробовать `oem log` + `get_staged` дважды.

### 2026-11-09, день: паника на маунте и операции записи закрыты

**Паника `Mutex ... unexpectedly not owned` — не в записи.** Символизация стека по
`mach.development.bcm2837.unstripped` дала: `apfs_mount` → `apfs_mount_common` →
`apfs_spaceman_open` → `apfs_extent...` → `nx_block_unlocked` → `nx_unlock`. То есть во время
*mount* `apfs_spaceman_open()` (statfs, `f_bfree`) читал блок без `am_lock`. Если блока не было
в кэше, `nx_block_unlocked` снимал `am_lock`, которого поток не держал. На другом прогоне блок
уже лежал в кэше — отсюда «гонка». Правка: `apfs_lock(am)` вокруг `apfs_spaceman_open/close`
в `apfs_mount_common` (после открытия тома, как все прочие вызовы в `apfs_lib`).

**Запись файлов.** Введён `apfs_write_data()` (`apfs_fs_write.c`, `op_write`) — один
транзакционный путь на любой offset:
- блок уже принадлежит файлу — читается, патчится, ставится обратно;
- блок в дырке (`phys=0`) или за последним extent — выделяется, и extent-записи вокруг него
  переделываются так, чтобы поток шёл от нуля без разрывов (apfsck этого требует);
- `alloced_size` = сумма длин всех extent-записей, `sparse_bytes` xfield = сумма дырок,
  плюс `APFS_INODE_IS_SPARSE`;
- список extent'ов на время операции ведётся в памяти (`fs_ext_list`), потому что
  `apfs_extent_next()` читает только закоммиченное дерево и не видит записи этой транзакции;
- соседние свежие блоки сливаются в один extent-рекорд.

`apfs_vnop_write()` теперь зовёт `apfs_write_data()`; `O_APPEND` разворачивается вручную
(`IO_APPEND`): VFS передаёт флаг, а не offset в конец. После прямой записи страницы UBC в
записанном диапазоне сбрасываются, перед записью грязные страницы выталкиваются.
Добавлены `vnop_pageout` (mmap → extents), `vnop_link`, `vnop_symlink` (xattr
`com.apple.fs.symlink`, флаг `APFS_XATTR_FILE_SYSTEM_OWNED`), grow в `op_truncate`
(дырка + sparse xfield), корректное удаление одного из имён хардлинка (inode остаётся,
имя primary переезжает на оставшуюся ссылку) и счётчики file/symlink в суперблоке.

**Проверки.** `tools/apfs/write-tests.py`: новый тест `ops` (grow, hard link, symlink,
truncate+extend) и команды `txn-link`/`txn-symlink` в `apfs-tool`; весь набор — **134/0**.
QEMU `apfs_rw_test`: добавлен тот же набор операций — `RESULT: 0 failure(s)`, apfsck чистый.
На телефоне самопроверки init на свежем томе: все write-операции проходят (create/append/
overwrite/truncate+extend/rename/hard link/symlink/mmap), остаются только прежние неудачи
чтения system image (`read 300 KB`, `seek middle`, `mmap page-in`, `exec`) — это отдельная
история dyld/system volume.

**Почему `klog_reset` не работал.** `loader_log_reset()` чистил `loader_log`, но
`loader_return()` в конце добавляет к нему весь kernel msgbuf (`loader_log_append_msgbuf`),
поэтому ранняя загрузка возвращалась снова. Теперь reset обнуляет и `msgbufp->msg_bufx/r`
(помощник `loader_log_reset_msgbuf`), так что после сброса в логе остаётся только то, что
написано после него.

**Сброс лога теперь по флагу.** Как только reset заработал, выяснилось, что он стирает
самопроверки: они печатаются до `klog_reset`, а в лог попадает только `run_rc` и
`init: done`. Поэтому `init` делает сброс, только если в образе есть `/etc/klog_reset`
(`stat(in_image("/etc/klog_reset"), &st) == 0`). `mk8.sh` создаёт файл только для
`phone8boot` (там нужен хвост dyld), а `phone8seed` и `phone8test` его не получают и
сохраняют весь вывод.

**Seed на телефоне.** `rc.jobs` в ramdisk вообще не исполнялся: `/etc/rc` образа делает
`exec /sbin/launchd.ios`. Поэтому `seed force` не пересевал том, и снятый дамп показывал
xid 1271 с прошлой сессии. Для проверок сделаны payload'ы `mk8.sh`: один с `/etc/rc` =
команда `apfs_rw_test ... seed force`, второй с пустым `/etc/rc` (init проходит самопроверки
и завершает загрузку, не отдавая процесс launchd).

**Отладка убрана.** `apfs_why_append()` и все вызовы (в apfs и трёх файлах VFS),
`kern.apfs_mounts`, счётчик `apfs_writable_checks`, пробы `.rwprobe`/`kern.apfs_stats` в init.
`kern.apfs_stats` оставлен как обычная статистика (без счётчика проверок записи).

**dyld/launchd: что выяснено (2026-11-09, день).** По разбору с Gemini 4 (Argon):
`/usr/lib/libSystem.B.dylib` на образе **действительно отсутствует** (есть только
`libSystem.B_asan.dylib`); на iOS 16 он должен браться из dyld shared cache. Сам кэш лежит не в
`/System/Library/Caches/...`, а в Cryptex-оверлее:
`/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64*` — и в APFS
он есть, читается. Версия про путаницу регистра в APFS-lookup не подтвердилась: `/system/...`
даёт ENOENT, `/System/...` открывается. Значит dyld либо не идёт в Cryptex-путь (ignition не
срабатывает), либо не мапит кэш. Для проверки в окружение PID 1 (init, `run_command`) добавлены
`DYLD_SHARED_REGION=private`,
`DYLD_SHARED_CACHE_DIR=/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld`,
`DYLD_PRINT_TO_FILE=/private/var/dyld.log`, `DYLD_PRINT_SEARCHING=1`, `DYLD_PRINT_SEGMENTS=1`.
Payload `phone8boot` (`/etc/rc` = `exec /sbin/launchd.ios`) проверяет этот путь: если заработает —
дальше смотреть `dyld.log` и решать, нужен ли `skipIgnition` в commpage или свой маунт Cryptex.

**Seed новым ядром (2026-11-09, день).** `phone8seed`: `seed list: 110 records, 110325760 bytes
at 8589934592`, `ok write the container image`, `RESULT: 0 failure(s)`, затем `=== init: done ===`.
Том после seed чистый; init-самопроверки гоняются отдельным payload'ом с пустым `/etc/rc`.

**Итог прогона `phone8test` (2026-11-09, ночь).** Сброс по флагу работает: лог дошёл до
`=== 26 checks, 5 failed ===` (раньше сброс стирал всё до `init: done`). Пять провалов —
прежние чтения system image (`graft prefix`, `read 300 KB`, `seek middle`, `mmap page-in`,
`exec`), к записи отношения не имеют. Но **write-самопроверки не запустились**: `test_writable`
выходит сразу, если не удался `mkdir("/tmp/t")`. Добавлена печать ошибки в эту ветку; следующий
прогон покажет errno (скорее всего `/tmp/t` остался от прошлого прогона или `/tmp` не ведёт
на `/private/var/tmp`). После этого — прогнать `phone8boot` с `DYLD_*` и смотреть `dyld.log`.



### 2026-11-09, день-2: write-самопроверки зелёные (39/0)

**`mkdir("/tmp/t")` падал с `EEXIST`**, а не с `EROFS`: том данных теперь переживает загрузку, и
`/tmp/t` остался от прошлого прогона. `init` печатает errno, а `test_writable` при `EEXIST`
убирает имена, которые сам же создаёт (`sweep_test_dir`), и идёт дальше.

**Hard link.** После `unlink` одного из двух имён `fstat` показывал то 0, то 2. Причина —
`apfs_vnop_remove` звал `apfs_node_refresh(am, vp)` вместо `apfs_node_refresh(am, APFS_VP(vp))`:
указатель на vnode читался как `struct apfs_node`, refresh читал мусор (`an_id` = `0xDEADB`) и
писал `an_ino` в чужую память. Второй баг: флаг `an_unlinked` («имени больше нет, инод ждёт
последнего close») ставился и на промежуточном удалении; теперь только когда ушло последнее имя
(`an_ino.nchildren <= 1`).

**Удаление последнего имени открытого файла — отложенное.** `fs_remove_file` получил флаг
`keep`: при нём у инода `nchildren = 0`, запись каталога уходит, а инод, экстенты и xattr'ы
остаются; `VNOP_INACTIVE` при последнем close зовёт `apfs_delete_inode` (новый
`op_orphan_delete`). `va_nlink` у открытого «удалённого» файла — 0.

**Сокет.** В таблице операций не было `VNOP_MKNOD`; bind AF_UNIX отбивался `EPERM` от
`vn_default_error`. Добавлен `apfs_vnop_mknod` для `VSOCK`/`VFIFO`; тип берётся из `va_type`,
а не из `va_mode` (VFS кладёт его туда).

**Graft `/var/jb` на постоянном томе.** `mkdir`/`symlink` при `EEXIST` больше не валят graft:
дерево прошлой загрузки — те же имена, указывающие на тот же `/jb`. Первый graft — ~7000
отдельных транзакций, около 8 минут; повторный проход только проверяет `EEXIST`.

**Те 4 «чтения system image» — не system image.** `read 300 KB`, `seek`, `mmap` читают
`/redwood/share/pattern.bin`, а `exec` — `/redwood/bin/child`; в payload-образе этих файлов не
было (пустой `share/`). Вернул `pattern.bin` (формула из `build.sh`) и `child` (собран
`xnu-clang` + libmini); `mk8.sh` берёт их из `~/apfs-test/root-tree`.

**Проверено.** Телефон: `=== 39 checks, 0 failed ===` — graft, все write-операции и чтения;
хостовые `tools/apfs/write-tests.py` — 134/0. Прогон занимает ~5–6 минут: «300 файлов» — это
300 созданий и 300 удалений, каждое отдельная транзакция с коммитом на флеш.

**Дальше.** `phone8boot` с `DYLD_*`, разбор `/private/var/dyld.log` (дамп тома `dump48.ps1` +
`readvar.sh`), затем решение по Cryptex/skipIgnition. После правок ядра — `xnu/export.sh`,
чтобы `xnu/files` и `hakios.patch` совпали с деревом `xnu-src/xnu-8796.121.2-raphael`.

### 2026-11-09, вечер: dyld и launchd поехали; launchd убивал 50-мегабайтный лимит

**Как читать полный лог.** `get_staged` отдавал 32 КБ, потому что `oem log` пересобирает
staged-буфер из одного loader-лога (`compose_logs(NULL, 0)`). После возврата ядра `write_logs()`
уже положил туда **весь** лог (loader + kernel); надо звать `get_staged` *без* `oem log` — так
пришёл лог на 170 КБ, с хвостом, паникой и стекшотом.

**dyld находит кэш, но private-маппинг убивает launchd.** С `DYLD_SHARED_REGION=private` dyld
отмапил все 43 файла кэша из `/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/`,
ignition прошёл, launchd.ios стартовал — и через ~2 минуты:

```
EXC_RESOURCE -> launchd.ios[1] exceeded mem limit: ActiveHard 50 MB (fatal)
memorystatus: killing process 1 [launchd.ios] in high band IMPORTANT (180)
panic: initproc exited -- exit reason namespace 1 subcode 0x7
```

2.6 ГБ кэша, отмапленные в процесс, пробили лимит launchd. **Правка:** убрать
`DYLD_SHARED_REGION`/`DYLD_SHARED_CACHE_DIR` из окружения `exec` в `init.c` — dyld берёт кэш по
своему дефолтному пути (этот же каталог) и просит у ядра shared-регион; страницы кэша процессу
не засчитываются.

**После правки:** ядро не возвращается, паники нет. launchd:
`Darwin Bootstrapper Version 7.0.0`, `Restore environment starting`, `Early boot complete.
Continuing system boot.`, домен `user/501`, демо-джоб `com.hakios.demo` спавнится (`pid 4`) и
выходит `exit(0)`. Не хватает `/System/Library/xpc/launchd.plist` (`Unable to open ...`), поэтому
`launchd_cache_loader` скипается — в payload-дереве нет `xpc/`.

**Осталось:** launchd сидит в **restore**-режиме (boot tasks скипаются, SpringBoard/backboardd
не стартуют). Дальше — вывести его в обычный boot mode (skipIgnition / boot-mode / свой маунт
Cryptex); это следующая сессия.

**Как гонять payload'ы.** В одном UEFI-лоадере: `oem stop`, дождаться `getvar result` != `running`,
потом `oem ramdisk` + `stage flashboot.apfs` + `stage payload` + `oem run`. Ребут в бутлоадер из
лоадера = холодный сброс в Android (там бутлуп). `skip_tests=1` в cmdline пропускает
самопроверки — `phone8boot` доходит до launchd за ~1,5 мин вместо 6.

### 2026-11-09, вечер-2: SpringBoard стартует; том данных ловит двойное освобождение

**Boot mode launchd.** Не NVRAM и не boot spec: launchd читает `kern.bootargs` (sysctl поверх
`PE_boot_args()`, менять нельзя) и ищет в строке `rd=md0`; плюс смотрит `ReleaseType` в
`/System/Library/CoreServices/SystemVersion.plist` на `Restore`/`Ramdisk`/`RamDisk`. `rd=md0`
добавляет сам лоадер (`xnuloader/loader.c`, «the root is the first RAM disk unless the payload says
otherwise»); ядро по нему только пропускает `FSIOC_KERNEL_ROOTAUTH` (`bsd_rooted_ramdisk`).
**Restore-режим — не причина:** в логе рабочего прогона 2026-11-07 SpringBoard жил с тем же
`Restore environment starting`. Поэтому `rd=`/ignition не трогали.

**Настоящая причина, почему SpringBoard не стартовал.** В payload-дереве
`~/apfs-test/root-tree/LaunchDaemons` лежал один `com.hakios.demo.plist`; полный набор джобов и
`libhakios.*` остался в старом `~/hakios/work-drivers/root-tree`. Полное дерево + текущие
`init`/`kernel` — SpringBoard стартует. Restore-режим при этом даже полезен: boot tasks
(`fsck`, `mount-phase-*`, `data-protection`) скипаются и не лезут в синтетическую систему.

**backboardd: nil в `-[NSURL initFileURLWithPath:]`.** Через 10–30 с после старта backboardd
падал с `NSInvalidArgumentException: nil string parameter` и уносил SpringBoard (`exit 255`).
По стеку и дизассемблеру: `[MCM... pathProviderForSystemContainerForCurrentProcess]` →
`cachesPath` → nil (system-контейнера в `/private/var/containers/Data/System` нет, MCM его не
создаёт) → `-[NSURL initFileURLWithPath:]` с nil. Обход в `libhakios.dylib`
(`userland/src/inject.c`, `url_path_fixes()`): для backboardd подменяется IMP
`-[NSURL initFileURLWithPath:]`, nil заменяется на `/private/var/tmp`. С ним backboardd живёт,
`hakios: NSURL: a nil path became /private/var/tmp`, окно рисует и флипает
(`raphael-display: flip 1/2 latched`). Сборка библиотеки:
`cd ~/hakios/userland && source ../xnu-env.sh && xnu-clang -arch arm64 -isysroot
$HOME/theos/sdks/iPhoneOS16.5.sdk -O1 -dynamiclib -install_name /redwood/lib/libhakios.dylib
-framework CoreFoundation -framework IOKit -lobjc -o ~/apfs-test/libhakios.dylib src/inject.c`.

**Том данных: двойное освобождение после рабочей сессии.** После прогона, где система реально
писала (SpringBoard, логи, контейнеры), следующий boot даёт
`apfs: block N is given back but was free already` + `fs: commit failed: 5` (N плавает:
30413, 30319, 30301), и dyld не находит shared cache:
`Library not loaded: /usr/lib/libSystem.B.dylib (no such file, no dyld cache)` → паника
`initproc failed to start`. Это не сбой чтения system volume: система монтируется
(50 858 файлов), портится именно data-том. Первый boot после seed чистый, порча накапливается
за сессию. Лечение на сейчас — пересев (`phone8seed`), но это потеря постоянства.
`apfs_rw_test` научен: при `seed force` (`seed_only`) он больше не делает `mkdir(MPOINT)` —
на испорченном томе этот mkdir отдаёт EIO и не даёт пересеять. Правка в `userland/src/apfs_rw_test.c`.

**Где остановились.** После seed + `phone9boot`: SpringBoard и backboardd живы, окно сервера
рендерит и флипает, на экране всё ещё яблоко (boot-progress), UI не перехватил экран. Дальше:
(1) найти, почему первый кадр SpringBoard не заменяет яблоко (activation/lock screen?);
(2) или чинить двойное освобождение в APFS-записи, иначе каждый boot требует пересева.

**Инструменты.** `%TEMP%\opencode\mk9.sh` — payload `phone9boot` из полного дерева
(`boot-tree`), `libhakios.dylib` подкладывается из `~/apfs-test`; `build_rwtest.sh` — `apfs_rw_test`;
`run16.ps1` — прогон с опросом `getvar result` каждые 15 с (паника видна через 15 с, не через 3 мин);
`run17.ps1` — то же с `oem ramdisk none` перед повторной укладкой (не помогло, проблема была в томе).

**lsd: следующий в цепочке.** `lsd` тоже спрашивает контейнер; после обхода всех трёх вариантов
`initFileURLWithPath…` он больше не падает по nil, а выходит `exit(78)` (EX_CONFIG) и респавнится
раз в секунду — не может открыть/создать свою базу LaunchServices на свежем `/private/var`.
SpringBoard и backboardd при этом живы, экран — всё то же яблоко.
**Грабли ignition:** payload с `sleep 120` в `/etc/rc` и `skip_graft=1` в cmdline три раза подряд не доходил
до launchd (`Library not loaded: /usr/lib/libSystem.B.dylib`, «no dyld cache»); тот же payload без
`sleep`/`skip_graft` грузится. `skip_graft=1` оставлен только для seed-прогона (там он экономит
8 минут графта); в boot-payload его не ставить, `sleep` в rc не добавлять.

### 2026-11-09, вечер-3: lsd — это не exit(78); длинная нагрузка и молчаливый EIO

**lsd не выходит с 78 — это xpcproxy.** В логе launchd:

```
Service could not initialize: posix_spawn(/usr/libexec/lsd), error 0x2 - No such file or directory
initialization failure: 20F66: xpcproxy + 23708 [1097][...]: 0x2
xpcproxy exited due to exit(78)
```

То есть lsd вообще не запускается: `posix_spawn` в xpcproxy отвечает ENOENT (в XNU это
`LOAD_ENOENT` — не нашёлся либо сам файл, либо `/usr/lib/dyld`). Прежняя версия «lsd не открывает
базу LaunchServices» была неверной; обход nil в NSURL к этому не относится (он нужен, когда lsd
*запустится*).

**Что проверено на телефоне (payload `phone10boot`/`phone11boot`, три джобы в одном прогоне):**
- `com.hakios.lsd1` — plist как у lsd, но без `MachServices`, без `StandardErrorPath`, без
  `PublishesEvents`, программа `/usr/libexec/lsd` → тот же `posix_spawn ... 0x2`.
- `com.hakios.lsd3` — тот же plist, программа `/usr/bin/vm_stat` → **стартует** и печатает.
  Значит plist и окружение ни при чём; дело в файле или пути.
- `com.hakios.lsd4` — **байты lsd, положенные в рамдиск** (`/redwood/bin/lsd-copy`, взяты из
  `images/system/system.apfs` через `apfs-tool cat`) → **стартует** (`Successfully spawned
  lsd-copy[7]`, потом падает в общем SIGBUS-шторме).

Вывод на тот момент: сам бинарник исполним, а копия на системном томе — нет.

**Проба из `init` (payload `phone8test`, `lau_probe`) это опровергла.** В логе:

```
  lsd: open /usr/libexec/lsd -> 3, read 32, size 57424
  lsd: first bytes cf fa ed fe
  lsd: probe child pid 3
```

То есть с системного тома файл открывается, читается, первые байты — Mach-O, размер верный, и
`execve` в дочернем процессе **не вернул ошибку** (строки `lsd: execve ... -> 2` нет): из init,
от root и без sandbox, `/usr/libexec/lsd` исполняется. Значит ядро, VFS и чтение файла ни при
чём; отказывает именно `posix_spawn` внутри **xpcproxy**, и только для этого пути (копия тех же
байтов в рамдиске через тот же xpcproxy стартует). Осталось разделить две вещи: пользователя
(`UserName mobile` против root) и путь (системный том против рамдиска) — payload `phone13boot`
ставит четыре джобы в один прогон: `/usr/libexec/lsd` от mobile и от root, `/usr/libexec/nehelper`
от mobile и `/usr/bin/vm_stat` от mobile.

**Молчаливый EIO на вставке инод (новый симптом).** После длинной сессии (графт + демоны) том
данных начинает отвечать EIO на создание файлов:

```
apfs: fs: inode insert: 5
apfs: fs: operation refused: 5
apfs: create com.apple.xpc.activity2.plist in 1171: 5
```

51 раз за ~0,2 с, дальше том продолжает работать. `given back but was free already` при этом
**не** появляется — то есть это не то двойное освобождение. Похоже на ветку `btp_update_node`,
которая отдаёт EIO молча; в неё добавлены сообщения с причиной (`child oid ... is not in the
object map`, `block ... is not a node`, `block ... is not a tree root`, `block ... cannot be given
back`). Именно этот EIO, скорее всего, и есть причина SIGBUS-шторма: у процессов отваливаются
mmap-страницы из `/private/var`, и все они получают SIGBUS одновременно (в логе — восемь
процессов в пределах 0,8 мс, потом ещё пачки).

**Хост: длинная нагрузка в одной сессии.** Раньше все txn-команды `apfs-tool` открывали образ
заново на каждую операцию, поэтому состояние, которое держит *смонтированный* том (корни после
коммита, кэш object map), в тестах не жило. Добавлено:
- `apfs-tool txn-write IMAGE PATH OFFSET FILE|-` — путь ядра (`apfs_write_data`), которого в
  тестах не было вовсе;
- `apfs-tool batch IMAGE SCRIPT` — один процесс, образ открыт один раз, каждая строка скрипта —
  отдельная транзакция (`create/mkdir/rmdir/unlink/unlinkkeep/delete/rename/link/symlink/
  truncate/write/append/setxattr/rmxattr/mtime`);
- `tools/apfs/stress-ops.py` — случайная нагрузка (по процессу на шаг) и
  `tools/apfs/stress-batch.py` — та же по смыслу, но одним процессом, до 8000 шагов;
- `tools/apfs/build-tool.sh` берёт дерево из `XNU_SRC`, если он задан: до этого host-тесты
  собирались из базового `xnu-src/xnu-8796.121.2`, то есть **без** последних правок raphael-ветки.

**Найдено и исправлено на хосте (все — реальные баги записи):**
1. `append` в файл, который кончается дыркой, отдавал EIO (`ex.phys == 0`). `apfs_append_file`
   теперь идёт тем же путём, что ядро (`apfs_write_data` с offset = size), `op_append` удалён.
2. `truncate` не трогал `SPARSE_BYTES`/`alloced_size`: после «вырастить в дырку → обрезать»
   apfsck говорил `wrong count of sparse bytes` / `wrong allocated space`. Теперь sparse
   считается один раз до изменений и правится по мере отдачи записей, а `alloced` уменьшается на
   длину каждой отданной записи (apfsck ждёт в `alloced_size` сумму всех экстентов, включая дырки).
3. `truncate to 0` оставлял sparse-байты без dstream (`sparse bytes without dstream`) — теперь
   оба поля снимаются вместе.
4. При обрезке файла, который кончается дыркой, запись дырки оставалась длиннее файла — теперь
   она укорачивается до `trim_at`.

Проверено: `tools/apfs/write-tests.sh` — **134/0**; `stress-ops.py 500 --check-each` — чисто;
`stress-batch.py 8000` в одной сессии — чисто. Жалоба apfsck `wrong name for only link` на
жёстких ссылках остаётся: у наших ссылок нет sibling-записей в приватном каталоге, apfsck их ждёт.

**Теги освобождений в ядре.** `txn_free` получил `who`/`arg`, у каждого вызова свой:
`a tree node` (oid), `a file extent` (sid), `the tail of a truncated file` (sid), `an inode's
extents` (ino), `the commit retiring an old object` (0..8). Теперь сообщения такие:
- в одной транзакции: `block N is given back twice in one transaction: a tree node 0x.., then a file extent 0x..`;
- между транзакциями: `block N is given back but was free already: a file extent 0x.. asked for it`;
- адрес вне контейнера: `block N cannot be given back: <кто>, the container has M blocks`.

**UFS-лог.** `ufs_write()` возвращает список контроллера после **каждого** буфера, и обе строки
(`transfer list switched ...` / `... back to UEFI's`) писались всегда: по строке на блок коммита,
это заливает и лог (885 КБ за прогон), и экран. Успешные строки теперь под `ufs_trace=1`
(по умолчанию молчат); все неудачные ветки говорят как раньше.

**Паника «Invalid kernel stack pointer» и предел глубины дерева.** В прогоне `phone13boot`
ядро упало через ~85 с:

```
panic(cpu 0 caller 0xfffffff007aade68): Invalid kernel stack pointer (probable overflow)
Panicked task ...: pid 32: accountsd
```

Символы по `mach.development.bcm2837.unstripped` (`llvm-nm` + ближайший символ) дают кадры,
которые повторяются: `btp_update_node+0x490` → `btp_update_node+0x90` → … — то есть рекурсия
`btp_update_node` в себя. Предел был `depth > 16`, а кадр уровня большой (локальные `btp_node`,
`btp_out`, ключи/значения); 16 уровней + остальной стек не влезают в 16 КБ. Теперь предел
`BTP_MAX_DEPTH 6` (`depth >= 6`) — на этих томах дерево 3–4 уровня; при превышении в лог идёт
`tree too deep at block N (level D)`. Молчаливая прежде ветка «дерево слишком глубоко» теперь
срабатывает раньше, чем кончится стек, и говорит, на каком блоке это случилось.

**lsd при этом стартовал.** В том же прогоне все четыре джобы начались: `Successfully spawned
lsd[17]` (системный путь, mobile), `lsd[7]` (root), `nehelper[8]`, `vm_stat[10]` — и через
секунду попали в общий SIGBUS-шторм. То есть `posix_spawn ENOENT` для lsd — не постоянное
свойство пути, а состояние системы: в двух прогонах сразу после свежего seed он есть, на
уже-графтнутом томе его нет. Похоже на ту же нестабильность (page-in, отдающий отказ), а не на
отдельную проблему lsd.

### 2026-11-09, ночь: double-free устранён, p73-boot подтвердил стабильность

В sd/miscfs/apfs/apfs_fs_write.c и pfs_txn.c добавлен откат 
x->omap_addr = old_nx_omap и old_nx_next_oid при ошибках pfs_txn_commit.
В прогоне p73-boot на устройстве:
- Выполнено **231 760 записей (949 МБ)** на APFS-том userdata;
- 0 ошибок записи, 0 refused;
- 0 double-free («given back twice» / «free already»);
- 0 commit failed: 5.
Том больше не портится между перезагрузками.

### 2026-11-09, вечер-ночь: 48 ГБ смонтирован на флеше, побеждён ramfs-фоллбэк, коллизия next_obj_id

**1. Причина бага «1 ГБ в Настройках и краш Настроек»:**
В логах `p112-boot.log` выяснено, почему при загрузке `/private/var` сваливался в 1 ГБ `ramfs`:
```
apfs: volume 0: block 29217 is not a volume superblock
apfs_mount: apfs_mount_common returned 22
mount /dev/ufs_userdata,b=8192 on /private/var: Invalid argument; the memory filesystem instead
```
В APFS контейнер (блок 0) указывает дескрипторный пул чекпоинтов (`desc_base=1, desc_blocks=265`). Ядро при `apfs_nx_open` сканирует этот диапазон и выбирает чекпоинт с наибольшим `xid`.
На физическом UFS остался грязный чекпоинт от старого прогона с высоким XID (18590), где OMAP ссылался на блок 29217 (в чистом `data48b` не инициализирован).
Чистый образ `data48b.apfs` собран с `xid 1`, OMAP в блоке 26906, суперблок тома 0 — в блоке 26908.

**2. Зачистка чекпоинтов и полный ресид 48 ГБ контейнера:**
- В `data48b.chunks` записи 1 и 2 забиты нулями по блокам 3..265, уничтожая все старые чекпоинты.
- Пересобран `phone9seed-payload.bin` (21 762 184 байт) через `mkseed.sh`.
- Выполнен ресид `reseed.ps1` на телефон:
  `seed list: 119 records, 119332864 bytes at 8589934592 in /dev/rufs_userdata`
  `ok write the container image into the device`
  `RESULT: 0 failure(s)`.
  Чистый 48 ГБ APFS-контейнер физически записан на UFS userdata со смещения 8192 МБ.

**3. Успешный запуск и монтирование реального раздела:**
После ребута устройства скрипт `auto_boot_when_ready.ps1` подхватил fastboot, загрузил UEFI и ядро (`phone9boot-payload.bin`). В логе `p112-boot.log`:
```
apfs_mount: path=/dev/ufs_userdata base=8589934592 container=0 volume=-1
apfs_mount: VNOP_OPEN error=0 shared=0 rdev=0x200001e held=1
apfs_mount_common: devblock=4096 devblocks=29136891 base=8589934592 container=0 volume=-1
apfs_mount: apfs_mount_common returned 0
  /private/var: the APFS volume /dev/ufs_userdata,b=8192
[ ok ] mount the data volume on /private/var
  133 directories, 13 files
[ ok ] create the directories of the data volume
```
- Фоллбэк в ramfs побеждён: `/private/var` смонтирован с физического APFS-тома 48 ГБ на UFS.
- Wi-Fi модуль работает в штатном режиме (`wifi: TFTP read /readwrite/mcfg.tmp`).
- Демоны пользователя запущены: `mediaserverd` (pid 28), `datamigrator` (pid 29), `SpringBoard` (pid 6).

**4. Анализ первопричины сбоя транзакций и коллизии OID:**
- **Double-free и рассинхронизация spaceman:**
  В `apfs_txn.c:apfs_txn_commit()` переменная кольца эфемерных данных `nx->xp_data_next` не обновлялась в памяти (`nx->xp_data_next = (uint32_t)(data_at % nx->xp_data_blocks)`). Каждая последующая транзакция перезаписывала один и тот же слот в кольце эфемерных данных. При частичном сбое или откате битовая карта чанков рассинхронизировалась с реальными аллокациями (`block 29212 is given back but was free already`), приводя к `commit failed: 5`.
- **Коллизия OID узлов дерева и файлов:**
  Виртуальные узлы B-дерева и файлы в APFS делят одно пространство идентификаторов `next_obj_id` (через `btp_new_oid()`). При падении коммита суперблок тома оставался со старым значением `next_obj_id`, но ранее выделенные узлы дерева сохраняли свои высокие OID. При создании новых файлов драйвер выдавал уже занятые узлами OID, затирая записи в OMAP тома (`block 29143 is given back twice: tree node 0x1c23 (7203), then tree node 0x409 (1033)`) и создавая висячие записи каталогов (dangling drec).
- **Ошибка `disabled.plist in 60: 17`:**
  Инод 60 — это каталог `/private/var/db/com.apple.xpc.launchd`. Код 17 (`EEXIST`) вызван штатным `O_CREAT|O_EXCL` от launchd. Настоящий цикл вызывался последующими ошибками `ENOENT (2)` из-за недостижимых/висячих инодов.

**5. Реализованные исправления в ядре XNU и тестах:**
1. **Кольцо эфемерных данных (`apfs_txn.c`):**
   - Добавлено обновление `nx->xp_data_next`, `nx->xp_data_index` и `nx->xp_data_len` в `apfs_txn_commit()`.
   - Добавлена корректная обработка перехода через конец кольца (wrap-around) в `txn_ephemerals()`.
2. **Динамический `id_floor` (`apfs_lib.c`, `apfs_lib.h`, `apfs_fs_write.c`):**
   - Вместо жесткого сдвига внедрён динамический `id_floor = max(sb.next_obj_id, max_fs_oid + 1, max_omap_oid + 1, nx.next_oid, APFS_MIN_USER_INO_NUM)`.
   - Реализован быстрый поиск максимального OID `apfs_btree_max_key_oid()` через обход правых листьев B-дерева за O(depth).
   - Чтение `id_floor` выполняется до открытия деревьев, чтобы не читать освобождённый буфер кэша суперблока.
   - Для ключей OMAP поддержано полное 64-битное извлечение OID (`tree->key_size >= 16 ? le64(k) : jkey_id(k)`).
   - Добавлена проверка `if (nx->sb != NULL)` для безопасной работы read-only маунтов.
   - В `fs_add_file()` добавлен цикл ретрая при `EEXIST` (пропуск дырок/orphan-инодов).
3. **Безопасная защита от повреждения тома (`apfs_fs_write.c`):**
   - В `fs_run()` при любой ошибке `fs_commit()` драйвер немедленно сбрасывает запись (`vol->nx->dev.write = NULL;`), переводя том в read-only и предотвращая цепное разрушение структур.
4. **Сборка и упаковка:**
   - Инструменты хоста `apfs-tool-patched` и `verify-patched` подтвердили 0 dangling, 0 unreachable даже при искусственно заниженном счётчике.
   - Ядро XNU (`mach.development.bcm2837`) скомпилировано чисто.
   - Упакован новый `phone9boot-payload.bin` (37 779 080 байт, 398 jobs, 10 libs) через `mk9.sh` и выложен в `~/apfs-test` и `%TEMP%\opencode`.
   - Задачи разделены: Antigravity отвечает за ядро/компиляцию/payload, DeepSeek отвечает за fastboot-тесты на телефоне Mi 9T Pro.
