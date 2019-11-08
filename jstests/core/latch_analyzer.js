/**
 * Verify that the LatchAnalyzer is working to expectations
 * @tags: []
 */

(function() {
"use strict";

const failPointName = "enableLatchAnalysis";

function getLatchAnalysis() {
    let serverStatus = assert.commandWorked(db.serverStatus({latchAnalysis: 1}));
    return serverStatus.latchAnalysis;
}

function verifyField(fieldValue) {
    // Every field in the latchAnalysis object is an integer that is greater than zero
    // This function is meant to verify those conditions

    assert.neq(fieldValue, null);
    assert(typeof fieldValue == 'number');
    assert(fieldValue >= 0);
}

function verifyLatchAnalysis({analysis, shouldHaveHierarchy}) {
    assert(analysis);

    if(shouldHaveHierarchy){
        jsTestLog("Failpoint is on; latch analysis: " + tojson(analysis));
    }else {
        jsTestLog("Failpoint is off; should be only basic stats: " + tojson(analysis));
    }

    for (var key in analysis) {
        let singleLatch = analysis[key]
        verifyField(singleLatch.acquired);
        verifyField(singleLatch.released);
        verifyField(singleLatch.contended);

        const acquiredAfter = singleLatch.acquiredAfter;
        const releasedBefore = singleLatch.releasedBefore;
        if(!shouldHaveHierarchy) {
            assert(!acquiredAfter);
            assert(!releasedBefore);
            continue;
        }

        for(var otherKey in acquiredAfter) {
            verifyField(acquiredAfter[otherKey]);
        }

        for(var otherKey in releasedBefore) {
            verifyField(releasedBefore[otherKey]);
        }
    }
}

try {
    {
        let analysis = getLatchAnalysis();
        verifyLatchAnalysis({analysis: analysis, shouldHaveHierarchy: false});
    }

    assert.commandWorked(db.adminCommand({
        configureFailPoint: failPointName,
        mode: "alwaysOn",
    }));

    {
        let analysis = getLatchAnalysis();
        verifyLatchAnalysis({analysis: analysis, shouldHaveHierarchy: true});
    }

    assert.commandWorked(db.adminCommand({
        configureFailPoint: failPointName,
        mode: "off",
    }));

    {
        let analysis = getLatchAnalysis();
        verifyLatchAnalysis({analysis: analysis, shouldHaveHierarchy: false});
    }

    /*

    let analysis1 = getLatchAnalysis();

    // Give some time to theoretically have latches change...but we shouldn't be tracking
    sleep(1000);
    let analysis2 = getLatchAnalysis();

    jsTestLog("Failpoint is off; latch analyses should be identical: \n" + tojson(analysis1) +
    "\n======\n" + tojson(analysis2)); verifyLatchAnalysis(analysis1); assert.eq(analysis1,
    analysis2);

    assert.commandWorked(db.adminCommand({
        configureFailPoint: failPointName,
        mode: "alwaysOn",
    }));

    let analysis3 = getLatchAnalysis();

    jsTestLog("Failpoint is on; latch analyses should be different: \n" + tojson(analysis1) +
    "\n======\n" + tojson(analysis3)); verifyLatchAnalysis(analysis3); assert.neq(analysis1,
    analysis3);
    */
} finally {
    assert.commandWorked(db.adminCommand({
        configureFailPoint: failPointName,
        mode: "off",
    }));
}
})();
