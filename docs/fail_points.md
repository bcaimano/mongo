# Fail Points

Fail points are test-only configurable hooks that can be triggered at runtime. Fail points allow
tests to change behaviour at pre-defined points to block threads, chose rarely executed branches,
enhance diagnostics, or achieve any number of other aims. Fail points can be enabled, configured,
and/or disabled via command to a remote node or via an API within the same process.

## Using Fail Points

A fail point must first be defined using `MONGO_FAIL_POINT_DEFINE(myFailPoint)`. This statement
merely adds the fail point to a registry. It can now be configured, waited upon, and evalutated in
code.  However, this is not enough to allow a user to modify runtime behavior. The fail point also
needs to be evaluated in code. There are three common patterns for using a fail point:
- If the fail point is set, chose to perform a certain behavior:
  `if (myRareCondition || myFailPoint.shouldFail()) { ... }`
- If the fail point is set, block until it is unset:
  `myFailPoint.pauseWhileSet();`
- If the fail point is set, use its payload to perform custom behavior:
  `myFailPoint.execute([](const BSONObj& obj) { ...  };`

For more complete usage, see the [fail point header][fail_points] or the [fail point
tests][fail_point_test].

## Configuring and Waiting on Fail Points

Fail points configuration involves chosing a "mode" for activation (e.g., "alwaysOn") and optionally
providing additional data in the form of a BSON object. For the vast majority of cases, this is done
by issuing a `configureFailPoint` command request from a javascript test. This is made easier using
the `configureFailPoint` helper from [fail_point_util.js][fail_point_utils]. However, fail points
can also be useful in C++ unit tests and integration tests. To configure fail points on the local
process, use a `FailPointEnableBlock` to enable and configure the fail point for a given block
scope.

Similarly to configuration, users can wait until a fail point has been evaluated a certain number of
times _*over its lifetime*_. In javascript tests, a `waitForFailPoint` command request will send
a response back when the fail point has been evaluated the given number of times. The
`configureFailPoint` helper returns an object that can be used to wait a certain amount of times
_*from when the fail point was enabled*_. In C++ tests, users can invoke
`FailPoint::waitForTimesEntered()` for similar behavior. `FailPointEnableBlock::initialTimesEntered`
is the amount of times the fail point had been evaluated when the `FailPointEnableBlock` was
constructed.

For javascript examples, see the [javascript fail point test][fail_point_javascript_test]. For the
command implimentations, see [here][fail_point_commands].

## The `failCommand` Fail Point

The `failCommand` fail point is an especially developed fail point used to mock arbitrary response
behaviors to requests filtered by command, appName, etc. It is most often used to simulate specific
topology conditions like a failed node or illegal replica set configurations. For examples of use,
see the [failCommand javascript tests][fail_command_javascript_test].

[fail_point]: ../src/mongo/util/fail_point.h
[fail_point_test]: ../src/mongo/util/fail_point_test.cpp
[fail_point_commands]: ../src/mongo/db/commands/fail_point_cmd.cpp
[fail_point_util]: ../jstests/libs/fail_point_util.js
[fail_point_javascript_test]: ../jstests/fail_point/fail_point.js
[fail_command_javascript_test]: ../jstests/core/failcommand_failpoint.js
