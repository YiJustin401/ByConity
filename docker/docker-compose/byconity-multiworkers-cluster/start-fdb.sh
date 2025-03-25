#! /bin/bash
set -eu

# Attempt to connect. Configure the database if necessary.
FDB_CLUSTER_FILE=/config/fdb.cluster

# if ! fdbcli -C $FDB_CLUSTER_FILE --exec status --timeout 5 ; then
#     if ! fdbcli -C $FDB_CLUSTER_FILE --exec "configure new single ssd ; status" --timeout 10 ; then
#         echo "Unable to configure new FDB cluster."
#         exit 1
#     fi
# fi

while [ $RETRY_COUNT -lt $MAX_RETRIES ]; do
    if fdbcli -C $FDB_CLUSTER_FILE --exec status --timeout 10; then
        echo "成功连接到 FDB 集群"
        break
    else
        RETRY_COUNT=$((RETRY_COUNT+1))
        echo "尝试 $RETRY_COUNT/$MAX_RETRIES: 连接 FDB 失败，尝试配置..."
        
        if fdbcli -C $FDB_CLUSTER_FILE --exec "configure new single ssd ; status" --timeout 15; then
            echo "FDB 集群配置成功"
            break
        fi
        
        if [ $RETRY_COUNT -eq $MAX_RETRIES ]; then
            echo "无法配置 FDB 集群，达到最大重试次数"
            exit 1
        fi
        
        echo "等待 5 秒后重试..."
        sleep 5
    fi
done

echo "Can now connect to docker-based FDB cluster using $FDB_CLUSTER_FILE."
