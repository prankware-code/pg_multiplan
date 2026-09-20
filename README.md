# pg_multiplan
Contrib pg_multiplan for postgresql

собрать:

```bash
./configure --prefix=$HOME/pgsql --enable-debug --enable-cassert CFLAGS="-O0 -g"

make -j6
make install
```

собрать расширение:

```bash
cd contrib/pg_multiplan; make PG_CONFIG=$HOME/pgsql/bin/pg_config install; cd ../..
```

запустить
```bash
export LD_LIBRARY_PATH=$HOME/pgsql/lib:$LD_LIBRARY_PATH
echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope

$HOME/pgsql/bin/pg_ctl -D $HOME/pgsql/data -l $HOME/pgsql/logfile restart

$HOME/pgsql/bin/psql postgres
```
