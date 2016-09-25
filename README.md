#Code with DIR_SYNC disabled

Clone  -  https://github.com/SudarsunKannan/sqlite3-3.8.2

    $ ./configure --enable-load-extension 
    $ make

If you don't want to change your existing python libraries better to run 

LD_PRELOAD= $SQLITESOURCE/.libs/libsqlite3.so.0  python bench.py ...
where  $SQLITESOURCE is the SQLITE code directory.


If you fine with replacing you SQLite and python libraries, then you can do 

    $make && $sudo make install


Note that, there are several optimizations since 2013 (~ 50 versions)
