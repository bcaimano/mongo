load("jstests/libs/parallelTester.js");

/**
 * @tags: [requires_sharding]
 */

(function() {
    "use strict";

    const st = new ShardingTest({mongos: 1, shards: 1, rs: { nodes: 3, protocolVersion:1 }});
    const kDbName = 'test';
    const mongosClient = st.s;
    const mongosDB = mongosClient.getDB(kDbName);

    function configureReplSetFailpoint(modeValue) {
        st.rs0.nodes.forEach(function(node){
            assert.commandWorked(node.getDB("admin").runCommand(
                {configureFailPoint: "waitInFindBeforeMakingBatch", mode: modeValue,
                             data: {shouldCheckForInterrupt: true},
                }));
        });
    };

    var threads = [];
    function launchFinds({times, readPref}){
        for(var i=0; i < times; i++){
            var thread = new Thread(function(connStr,readPref, dbName){
                jsTestLog("Starting connection to " + connStr);
                var client = new Mongo(connStr);
                assert.commandWorked(client.getDB(dbName).runCommand(
                    {find: "test", limit: 1, "$readPreference": {mode: readPref}}
                ));
                jsTestLog("Done with " + connStr);
                },
                st.s.host,
                readPref,
                kDbName);
            thread.start();
            threads.push(thread);
        }
    };

    assert.writeOK(mongosDB.test.insert({x: 1}));
    assert.writeOK(mongosDB.test.insert({x: 2}));
    assert.writeOK(mongosDB.test.insert({x: 3}));
    st.rs0.awaitReplication();

    // Shard the collection
    /*
    assert.commandWorked(mongosDB.test.ensureIndex({x: 1}));
    const shardCommand = {shardcollection: "test.test", key: {x: 1}};
    assert.commandFailed(st.s.adminCommand(shardCommand));
    assert.commandWorked(st.s.adminCommand({enablesharding: "test"}));
    */

    jsTestLog(mongosDB.runCommand( { "connPoolStats" : 1 } ));

    configureReplSetFailpoint("alwaysOn");

    launchFinds({times: 10, readPref: "primary"});

    sleep(1000);

    jsTestLog(mongosDB.runCommand( { "connPoolStats" : 1 } ));

    configureReplSetFailpoint("off");

    threads.forEach(function(thread){
        thread.join();
    });

    st.stop();
})();
