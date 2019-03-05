mkdir shadow
#mysql-proxy --defaults-file=./mysql-proxy.cnf --proxy-lua-script=`pwd`/wrapper.lua

mysql-proxy --plugins=proxy --event-threads=4 --max-open-files=1024 --proxy-lua-script=/opt/cryptdb/wrapper.lua --proxy-address=172.18.0.3:3399 --proxy-backend-addresses=172.18.0.2:3306
