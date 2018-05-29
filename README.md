# vsam
This NodeJS module enables you to read VSAM files on z/OS

## Installation

<!--
This is a [Node.js](https://nodejs.org/en/) module available through the
[npm registry](https://www.npmjs.com/).
-->

Before installing, [download and install Node.js](https://developer.ibm.com/node/sdk/ztp/).
Node.js 6.11.4 or higher is required.

## Simple to use

Vsam.js is designed to be a bare bones vsam I/O module.

```js
try {
  file = vsam.open("//'sample.test.vsam.ksds'",
                   JSON.parse(fs.readFileSync('schema.json')));
  file.find("0321", (record, err) => {
      if (err != null)
        console.log("Not found!");
      else {
        assert(record.key, "0321");
        console.log(`Current details: Name(${record.name}), Gender(${record.gender})`);
        record.name = "KEVIN";
        file.update(record, (err) => {
          file.close();
        });
      }
    }
);
```
schema.json looks like this:

```json
{
  "key": {
    "type": "string",
    "maxLength": 5
  },
  "name": {
    "type": "string",
    "maxLength": 10
  },
  "gender": {
    "type": "string",
    "maxLength": 10
  }
}
```

## Table of contents

- [Opening a vsam file for I/O](#opening-a-vsam-file-for-io)
- [Closing a vsam file](#closing-a-vsam-file)
- [Reading a record from a vsam file](#reading-a-record-from-a-vsam-file)
- [Writing a record to a vsam file](#writing-a-record-to-a-vsam-file)
- [Finding a record in a vsam file](#finding-a-record-in-a-vsam-file)
- [Updating a record in a vsam file](#updating-a-record-in-a-vsam-file)
- [Deleting a record from a vsam file](#deleting-a-record-from-a-vsam-file)
- [Deallocating a vsam file](#deallocating-a-vsam-file)

---

## Opening a vsam file for I/O

```js
const vsam = require('vsam');
const fs = require('fs');
var vsamObj = vsam.open( "//'VSAM.DATASET.NAME'", JSON.parse(fs.readFileSync('schema.json')));
```

* The first argument is the VSAM dataset name.
* The second argument is the JSON object derived from the schema file.
* The value returned is a vsam file handle. The rest of this readme describes the operations that can be performed on this object.
* Usage notes:
  * On opening a vsam file, the cursor is placed at the first record.
  * On error, this function with throw an exception.

## Closing a vsam file

```js
vsamObj.close((err) => { /* Handle error. */ });
```

* The first argument is the file descriptor.
* The second argument is a callback function containing an error object if the close operation failed.

## Reading a record from a vsam file

```js
vsamObj.read((record, err) => { 
  /* Use the record object. */
});
```

* The first argument is a callback function whose arguments are as follows:
  * The first argument is an object that contains the information from the next read record.
  * The second argument will contain an error object in case the read operation failed.
* Usage notes:
  * The read operation retrievs the record under the current cursor and advances the cursor by one record length.

## Writing a record to a vsam file

```js
vsamObj.write((record, (err) => { 
  /* Make sure err is null. */
}));
```

* The first argument is record object that will be written.
* The second argument is a callback to notify of any error in case the write operation failed.
* Usage notes:
  * The write operation advances the cursor by one record length after the newly written record.
  * The write operation will overwrite any existing record with the same key.

## Finding a record in a vsam file

```js
vsamObj.find((recordKey, (record, err) => { 
  /* Use record information. */
}));
```

* The first argument is record key (usually a string).
* The second argument is a callback
  * The first argument is a record object retrieved using the key provided.
  * The second argument is an error object in case the operation failed.
* Usage notes:
  * The find operation will place the cursor at the queried record (if found).
  * The record object in the callback will by null if the query failed to retrieve a record.
  
## Updating a record in a vsam file

```js
vsamObj.update((recordKey, (err) => { 
   ...
}));
```

* The first argument is record key (usually a string).
* The second argument is a callback
  * The first argument is an error object in case the operation failed.
* Usage notes:
  * The update operation will write over the record currently under the cursor.
  
## Deleting a record from a vsam file

```js
vsamObj.delete((err) => { /* Handle error. */ });
```

* The first argument is a callback function containing an error object if the delete operation failed.
* Usage notes:
  * The record under the current position of the file cursor gets deleted.
  * This will usually be placed inside the callback of a find operation. The find operation places
    the cursor on the desired record and the subsequent delete operation deletes it.

## Deallocating a vsam file

```js
vsamObj.dealloc((err) => { /* Handle error. */ });
```

* The first argument is a callback function containing an error object if the deallocation operation failed.