//
// DatabaseImpl.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "DatabaseImpl.hh"
#include "CollectionImpl.hh"
#include "c4Document.hh"
#include "c4Document.h"
#include "c4ExceptionUtils.hh"
#include "c4Internal.hh"
#include "c4Private.h"
#include "c4BlobStore.hh"
#include "TreeDocument.hh"
#include "VectorDocument.hh"
#include "BackgroundDB.hh"
#include "Housekeeper.hh"
#include "DataFile.hh"
#include "SQLiteDataFile.hh"
#include "Record.hh"
#include "SequenceTracker.hh"
#include "FleeceImpl.hh"
#include "Upgrader.hh"
#include "SecureRandomize.hh"
#include "StringUtil.hh"
#include "PrebuiltCopier.hh"
#include <functional>
#include <inttypes.h>
#include <unordered_set>



namespace litecore::constants {
    const C4Slice kLocalCheckpointStore   = C4STR("checkpoints");
    const C4Slice kPeerCheckpointStore    = C4STR("peerCheckpoints");
    const C4Slice kPreviousPrivateUUIDKey = C4STR("previousPrivateUUID");
}

namespace litecore {
    using namespace std;
    using namespace fleece;
    using namespace fleece::impl;


    static constexpr slice kMaxRevTreeDepthKey = "maxRevTreeDepth";
    static constexpr uint32_t kDefaultMaxRevTreeDepth = 20;


    static string collectionNameToKeyStoreName(slice collectionName);
    static slice keyStoreNameToCollectionName(slice name);


#pragma mark - OPENING / CLOSING:


    Retained<DatabaseImpl> DatabaseImpl::open(const FilePath &path, C4DatabaseConfig config) {
        Retained<DatabaseImpl> db = new DatabaseImpl(path, config);
        db->open(path);
        return db;
    }


    DatabaseImpl::DatabaseImpl(const FilePath &path, C4DatabaseConfig inConfig)
    :C4Database(path.unextendedName(), path.parentDir(), inConfig)
    ,_encoder(new Encoder())
    { }


    // `path` is path to bundle; return value is path to db file. Updates config.storageEngine. */
    /*static*/ FilePath DatabaseImpl::findOrCreateBundle(const string &path,
                                                         bool canCreate,
                                                         C4StorageEngine &storageEngine)
    {
        FilePath bundle(path, "");
        bool createdDir = (canCreate && bundle.mkdir());
        if (!createdDir)
            bundle.mustExistAsDir();

        DataFile::Factory *factory = DataFile::factoryNamed(storageEngine);
        if (!factory)
            error::_throw(error::InvalidParameter);

        // Look for the file corresponding to the requested storage engine (defaulting to SQLite):

        FilePath dbPath = bundle["db"].withExtension(factory->filenameExtension());
        if (createdDir || factory->fileExists(dbPath)) {
            // Db exists in expected format, or else we just created this blank bundle dir, so exit:
            if (storageEngine == nullptr)
                storageEngine = factory->cname();
            return dbPath;
        }

        if (storageEngine != nullptr) {
            // DB exists but not in the format they specified, so fail:
            error::_throw(error::WrongFormat);
        }

        // Not found, but they didn't specify a format, so try the other formats:
        for (auto otherFactory : DataFile::factories()) {
            if (otherFactory != factory) {
                dbPath = bundle["db"].withExtension(otherFactory->filenameExtension());
                if (factory->fileExists(dbPath)) {
                    storageEngine = factory->cname();
                    return dbPath;
                }
            }
        }

        // Weird; the bundle exists but doesn't contain any known type of database, so fail:
        error::_throw(error::WrongFormat);
    }


    void DatabaseImpl::open(const FilePath &bundlePath) {
        FilePath dataFilePath = findOrCreateBundle(bundlePath,
                                                   (_configV1.flags & kC4DB_Create) != 0,
                                                   _configV1.storageEngine);
        // Set up DataFile options:
        DataFile::Options options { };
        options.keyStores.sequences = true;
        options.create = (_config.flags & kC4DB_Create) != 0;
        options.writeable = (_config.flags & kC4DB_ReadOnly) == 0;
        options.upgradeable = (_config.flags & kC4DB_NoUpgrade) == 0;
        options.useDocumentKeys = true;
        options.encryptionAlgorithm = (EncryptionAlgorithm)_config.encryptionKey.algorithm;
        if (options.encryptionAlgorithm != kNoEncryption) {
#ifdef COUCHBASE_ENTERPRISE
            options.encryptionKey = alloc_slice(_config.encryptionKey.bytes,
                                                kEncryptionKeySize[options.encryptionAlgorithm]);
#else
            error::_throw(error::UnsupportedEncryption);
#endif
        }


        // Determine the storage type and its Factory object:
        const char *storageEngine = _configV1.storageEngine ? _configV1.storageEngine : "";
        DataFile::Factory *storageFactory = DataFile::factoryNamed((string)(storageEngine));
        if (!storageFactory)
            error::_throw(error::Unimplemented);

        // Open the DataFile:
        try {
            _dataFile.reset( storageFactory->openFile(dataFilePath, this, &options) );
        } catch (const error &x) {
            if (x.domain == error::LiteCore && x.code == error::DatabaseTooOld
                    && UpgradeDatabaseInPlace(dataFilePath.dir(), _configV1)) {
                // This is an old 1.x database; upgrade it in place, then open:
                _dataFile.reset( storageFactory->openFile(dataFilePath, this, &options) );
            } else {
                throw;
            }
        }

        if (options.useDocumentKeys)
            _encoder->setSharedKeys(_dataFile->documentKeys());

        // Validate or upgrade the database's document schema/versioning:
        _configV1.versioning = checkDocumentVersioning();

        if (_configV1.versioning == kC4VectorVersioning)
            _config.flags |= kC4DB_VersionVectors;
        else
            _config.flags &= ~kC4DB_VersionVectors;

        // Start document-expiration tasks for all Collections that need them:
        initCollections();
        startBackgroundTasks();
    }


    C4DocumentVersioning DatabaseImpl::checkDocumentVersioning() {
        //FIXME: This ought to be done _before_ the SQLite userVersion is updated
        // Compare existing versioning against runtime config:
        Record versDoc = getInfo("versioning");
        auto curVersioning = C4DocumentVersioning(versDoc.bodyAsUInt());
        auto newVersioning = _configV1.versioning;
        if (versDoc.exists() && curVersioning >= newVersioning)
            return curVersioning;

        // Mismatch -- could be a race condition. Open a transaction and recheck:
        Transaction t(this);
        versDoc = getInfo("versioning");
        curVersioning = C4DocumentVersioning(versDoc.bodyAsUInt());
        if (versDoc.exists() && curVersioning >= newVersioning)
            return curVersioning;

        // Yup, mismatch confirmed, so deal with it:
        if (versDoc.exists()) {
            // Existing db versioning does not match runtime config!
            upgradeDocumentVersioning(curVersioning, newVersioning, transaction());
        } else if (_config.flags & kC4DB_Create) {
            // First-time initialization:
            (void)generateUUID(kPublicUUIDKey);
            (void)generateUUID(kPrivateUUIDKey);
        } else {
            // Should never occur (existing db must have its versioning marked!)
            error::_throw(error::WrongFormat);
        }

        // Store new versioning:
        versDoc.setBodyAsUInt((uint64_t)newVersioning);
        setInfo(versDoc);
        t.commit();
        return newVersioning;
    }


    void DatabaseImpl::rekey(const C4EncryptionKey *newKey) {
        _dataFile->_logInfo("Rekeying database...");
        C4EncryptionKey keyBuf {kC4EncryptionNone, {}};
        if (!newKey)
            newKey = &keyBuf;

        mustNotBeInTransaction();
        stopBackgroundTasks();

        // Create a new BlobStore and copy/rekey the blobs into it:
        filePath().subdirectoryNamed("Attachments_temp").delRecursive();
        auto &blobStore = getBlobStore();
        auto newStore = createBlobStore("Attachments_temp", *newKey);
        try {
            blobStore.copyBlobsTo(*newStore);

            // Rekey the database itself:
            dataFile()->rekey((EncryptionAlgorithm)newKey->algorithm,
                              slice(newKey->bytes, kEncryptionKeySize[newKey->algorithm]));
        } catch (...) {
            newStore->deleteStore();
            throw;
        }

        const_cast<C4DatabaseConfig2&>(_config).encryptionKey = *newKey;

        // Finally replace the old BlobStore with the new one:
        blobStore.replaceWith(*newStore);
        startBackgroundTasks();
        _dataFile->_logInfo("Finished rekeying database!");
    }


    void DatabaseImpl::close() {
        mustNotBeInTransaction();
        stopBackgroundTasks();
        _dataFile->close();
    }


    void DatabaseImpl::closeAndDeleteFile() {
        mustNotBeInTransaction();
        stopBackgroundTasks();
        FilePath bundle = filePath().dir();
        _dataFile->deleteDataFile();
        bundle.delRecursive();
    }


    DatabaseImpl::~DatabaseImpl() {
        Assert(_transactionLevel == 0,
               "Database being destructed while in a transaction");

        destructExtraInfo(extraInfo);

        for (auto &entry : _collections)
            asInternal(entry.second.get())->close();

        FLEncoder_Free(_flEncoder);
        // Eagerly close the data file to ensure that no other instances will
        // be trying to use me as a delegate (for example in externalTransactionCommitted)
        // after I'm already in an invalid state
        if (_dataFile)
            _dataFile->close();
    }


#pragma mark - ACCESSORS:


    // Callback that takes a base64 blob digest and returns the blob data
    alloc_slice DatabaseImpl::blobAccessor(const Dict *blobDict) const {
        return getBlobStore().getBlobData(FLDict(blobDict));
    }


    uint32_t DatabaseImpl::maxRevTreeDepth() {
        if (_maxRevTreeDepth == 0) {
            _maxRevTreeDepth = uint32_t(getInfo(kMaxRevTreeDepthKey).bodyAsUInt());
            if (_maxRevTreeDepth == 0)
                _maxRevTreeDepth = kDefaultMaxRevTreeDepth;
        }
        return _maxRevTreeDepth;
    }

    void DatabaseImpl::setMaxRevTreeDepth(uint32_t depth) {
        if (depth == 0)
            depth = kDefaultMaxRevTreeDepth;
        Record rec = getInfo(kMaxRevTreeDepthKey);
        if (depth != rec.bodyAsUInt()) {
            Transaction t(this);
            rec.setBodyAsUInt(depth);
            setInfo(rec);
            t.commit();
        }
        _maxRevTreeDepth = depth;
    }


    C4BlobStore& DatabaseImpl::getBlobStore() const {
        if (!_blobStore)
            _blobStore = createBlobStore("Attachments", _config.encryptionKey);
        return *_blobStore;
    }


    unique_ptr<C4BlobStore> DatabaseImpl::createBlobStore(const string &dirname,
                                                          C4EncryptionKey encryptionKey) const
    {
	// Split path into a separate variable to workaround GCC 8 constructor resolution issue
	alloc_slice path = filePath().subdirectoryNamed(dirname);
        return make_unique<C4BlobStore>(path,_config.flags, encryptionKey);
    }


#pragma mark - HOUSEKEEPING:


    static_assert(int(kC4Compact)      == int(DataFile::kCompact));
    static_assert(int(kC4FullOptimize) == int(DataFile::kFullOptimize));


    void DatabaseImpl::maintenance(C4MaintenanceType what) {
        mustNotBeInTransaction();
        dataFile()->maintenance(DataFile::MaintenanceType(what));
        if (what == kC4Compact)
            garbageCollectBlobs();
    }


    void DatabaseImpl::garbageCollectBlobs() {
        // Lock the database to avoid any other thread creating a new blob, since if it did
        // I might end up deleting it during the sweep phase (deleteAllExcept).
        mustNotBeInTransaction();
        ExclusiveTransaction t(dataFile());

        unordered_set<C4BlobKey> usedDigests;
        auto blobCallback = [&](FLDict blob) {
            if (auto key = C4Blob::keyFromDigestProperty(blob); key)
            usedDigests.insert(*key);
            return true;
        };

        forEachCollection([&](C4Collection *coll) {
            asInternal(coll)->findBlobReferences(blobCallback);
        });

        // Now delete all blobs that don't have one of the referenced keys:
        auto numDeleted = getBlobStore().deleteAllExcept(usedDigests);
        if (numDeleted > 0 || !usedDigests.empty()) {
            LogTo(DBLog, "    ...deleted %u blobs (%zu remaining)",
                  numDeleted, usedDigests.size());
        }
    }


    BackgroundDB* DatabaseImpl::backgroundDatabase() {
        if (!_backgroundDB)
            _backgroundDB.reset(new BackgroundDB(this));
        return _backgroundDB.get();
    }


    void DatabaseImpl::stopBackgroundTasks() {
        // We can't hold the _collectionsMutex while calling stopHousekeeping(), or a deadlock may
        // result. So first enumerate the collections, then make the calls:
        vector<C4Collection*> collections;
        {
            LOCK(_collectionsMutex);
            for (auto &entry : _collections)
                collections.emplace_back(entry.second.get());
        }
        for (auto &coll : collections)
            asInternal(coll)->stopHousekeeping();

        if (_backgroundDB)
            _backgroundDB->close();
    }


    void DatabaseImpl::startBackgroundTasks() {
        for (const string &name : _dataFile->allKeyStoreNames()) {
            if (slice collName = keyStoreNameToCollectionName(name); collName) {
                if (_dataFile->getKeyStore(name).nextExpiration() > 0) {
                    asInternal(getCollection(collName))->startHousekeeping();
                }
            }
        }
    }


    C4Timestamp DatabaseImpl::nextDocExpiration() const {
        C4Timestamp minTime = 0;
        forEachCollection([&](C4Collection *coll) {
            auto time = coll->nextDocExpiration();
            if (time > minTime || minTime == 0)
                minTime = time;
        });
        return minTime;
    }


#pragma mark - UUIDS:


    bool DatabaseImpl::getUUIDIfExists(slice key, C4UUID &uuid) const {
        Record r = getInfo(key);
        if (!r.exists() || r.body().size < sizeof(C4UUID))
            return false;
        uuid = *(C4UUID*)r.body().buf;
        return true;
    }


    // must be called within a transaction
    C4UUID DatabaseImpl::generateUUID(slice key, bool overwrite) {
        C4UUID uuid;
        if (overwrite || !getUUIDIfExists(key, uuid)) {
            mutable_slice uuidSlice{&uuid, sizeof(uuid)};
            GenerateUUID(uuidSlice);
            setInfo(key, uuidSlice);
        }
        return uuid;
    }


    C4UUID DatabaseImpl::getUUID(slice key) const {
        C4UUID uuid;
        if (!getUUIDIfExists(key, uuid)) {
            auto self = const_cast<DatabaseImpl*>(this);
            Transaction t(self);
            uuid = self->generateUUID(key);
            t.commit();
        }
        return uuid;
    }


    void DatabaseImpl::resetUUIDs() {
        Transaction t(this);
        C4UUID previousPrivate = getUUID(kPrivateUUIDKey);
        setInfo(constants::kPreviousPrivateUUIDKey,
                {&previousPrivate, sizeof(C4UUID)});
        generateUUID(kPublicUUIDKey, true);
        generateUUID(kPrivateUUIDKey, true);
        t.commit();
    }


    uint64_t DatabaseImpl::myPeerID() const {
        if (_myPeerID == 0) {
            // Compute my peer ID from the first 64 bits of the public UUID.
            auto uuid = const_cast<DatabaseImpl*>(this)->getUUID(kPublicUUIDKey);
            memcpy(&_myPeerID, &uuid, sizeof(_myPeerID));
            _myPeerID = endian::dec64(_myPeerID);
            // Don't let it be zero:
            if (_myPeerID == 0)
                _myPeerID = 1;
        }
        return _myPeerID;
    }


    alloc_slice DatabaseImpl::getPeerID() const {
        char buf[32];
        sprintf(buf, "%" PRIx64, myPeerID());
        return alloc_slice(buf);
    }


#pragma mark - COLLECTIONS:


    static constexpr const char* kCollectionKeyStorePrefix = "coll_";

    static constexpr const char* kDefaultCollectionName = "_default";

    static constexpr slice kCollectionNameCharacterSet
                            = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890_-%";

    MUST_USE_RESULT
    static bool collectionNameIsValid(slice name) {
        // Enforce CBServer collection name restrictions:
        return name.size >= 1 && name.size <= 30
            && !name.findByteNotIn(kCollectionNameCharacterSet)
            && name[0] != '_' && name[0] != '%';
    }


    static string collectionNameToKeyStoreName(slice collectionName) {
        if (collectionName == kDefaultCollectionName) {
            return DataFile::kDefaultKeyStoreName;
        } else if (collectionNameIsValid(collectionName)) {
            // KeyStore name is "coll_" + name; SQLite table name will be "kv_coll_" + name
            string result = kCollectionKeyStorePrefix;
            result.append(collectionName);
            return result;
        } else {
            return {};
        }
    }


    static slice keyStoreNameToCollectionName(slice name) {
        if (name == DataFile::kDefaultKeyStoreName)
            return kDefaultCollectionName;
        else if (hasPrefix(name, kCollectionKeyStorePrefix)) {
            name.moveStart(strlen(kCollectionKeyStorePrefix));
            return name;
        } else {
            return nullslice;
        }
    }


    void DatabaseImpl::initCollections() {
        LOCK(_collectionsMutex);
        _defaultCollection = createCollection(kDefaultCollectionName);
    }


    bool DatabaseImpl::hasCollection(slice name) const {
        LOCK(_collectionsMutex);
        string keyStoreName = collectionNameToKeyStoreName(name);
        return !keyStoreName.empty()
            && (_collections.find(name) != _collections.end()
                || _dataFile->keyStoreExists(keyStoreName));
    }


    C4Collection* DatabaseImpl::getCollection(slice name) const {
        return const_cast<DatabaseImpl*>(this)->getOrCreateCollection(name, false);
    }

    C4Collection* DatabaseImpl::createCollection(slice name) {
        return getOrCreateCollection(name, true);
    }

    // This implements both the public getCollection() and createCollection()
    C4Collection* DatabaseImpl::getOrCreateCollection(slice name, bool canCreate) {
        LOCK(_collectionsMutex);
        if (!name)
            return _defaultCollection;                                      // -> Default coll.

        // Is there already a C4Collection object for it in _collections?
        if (auto i = _collections.find(name); i != _collections.end())
            return i->second.get();                                         // -> Existing object

        // Validate the name:
        string keyStoreName = collectionNameToKeyStoreName(name);
        if (keyStoreName.empty())
            C4Error::raise(LiteCoreDomain, kC4ErrorInvalidParameter,
                           "Invalid collection name '%.*s'", SPLAT(name));  //-> THROW

        // Validate its existence, if canCreate is false:
        if (!canCreate && !_dataFile->keyStoreExists(keyStoreName))
            return nullptr;                                                 //-> NULL

        // Instantiate it, creating the KeyStore on-disk if necessary:
        KeyStore &store = _dataFile->getKeyStore(keyStoreName);
        auto collection = make_unique<CollectionImpl>(this, name, store);
        // Update its state & add it to _collections:
        auto collectionPtr = collection.get();
        _collections.insert({collection->getName(), move(collection)});
        if (isInTransaction())
            collectionPtr->transactionBegan();
        return collectionPtr;                                               //-> New object
    }


    void DatabaseImpl::deleteCollection(slice name) {
        Transaction t(this);

        LOCK(_collectionsMutex);
        if (auto i = _collections.find(name); i != _collections.end()) {
            asInternal(i->second.get())->close();
            _collections.erase(i);
        }
        _dataFile->deleteKeyStore(collectionNameToKeyStoreName(name));

        t.commit();
    }


    vector<string> DatabaseImpl::getCollectionNames() const {
        vector<string> names;
        for (const string &name : _dataFile->allKeyStoreNames()) {
             if (slice collName = keyStoreNameToCollectionName(name); collName)
                names.emplace_back(collName);
        }
        return names;
    }


    void DatabaseImpl::forEachCollection(const function_ref<void(C4Collection*)> &callback) const {
        for (const auto &name : getCollectionNames()) {
            callback(getCollection(name));
        }
    }


    void DatabaseImpl::forEachOpenCollection(const function_ref<void(C4Collection*)> &callback) const {
        LOCK(_collectionsMutex);
        for (auto &entry : _collections)
            callback(entry.second.get());
    }

    
#pragma mark - TRANSACTIONS:


    void DatabaseImpl::beginTransaction() {
        if (++_transactionLevel == 1) {
            _transaction = new ExclusiveTransaction(_dataFile.get());
            forEachOpenCollection([&](C4Collection *coll) {
                asInternal(coll)->transactionBegan();
            });
        }
    }

    bool DatabaseImpl::isInTransaction() const noexcept {
        return _transactionLevel > 0;
    }


    void DatabaseImpl::mustBeInTransaction() {
        if (!isInTransaction())
            error::_throw(error::NotInTransaction);
    }


    void DatabaseImpl::endTransaction(bool commit) {
        if (_transactionLevel == 0)
            error::_throw(error::NotInTransaction);
        if (--_transactionLevel == 0) {
            auto t = _transaction;
            try {
                if (commit)
                    t->commit();
                else
                    t->abort();
            } catch (...) {
                _cleanupTransaction(false);
                throw;
            }
            _cleanupTransaction(commit);
        }
    }


    // The cleanup part of endTransaction
    void DatabaseImpl::_cleanupTransaction(bool committed) {
        forEachOpenCollection([&](C4Collection *coll) {
            asInternal(coll)->transactionEnding(_transaction, committed);
        });
        delete _transaction;
        _transaction = nullptr;
    }


    void DatabaseImpl::externalTransactionCommitted(const SequenceTracker &srcTracker) {
        // CAREFUL: This may be called on an arbitrary thread
        LOCK(_collectionsMutex);
        forEachOpenCollection([&](C4Collection *coll) {
            if (slice(asInternal(coll)->keyStore().name()) == srcTracker.name())
                asInternal(coll)->externalTransactionCommitted(srcTracker);
        });
    }


    void DatabaseImpl::mustNotBeInTransaction() {
        if (isInTransaction())
            error::_throw(error::TransactionNotClosed);
    }


    ExclusiveTransaction& DatabaseImpl::transaction() const {
        auto t = _transaction;
        if (!t) error::_throw(error::NotInTransaction);
        return *t;
    }


#pragma mark - INFO / RAW DOCUMENTS:


    KeyStore& DatabaseImpl::infoKeyStore() const {
        return _dataFile->getKeyStore(kInfoStore, KeyStore::noSequences);
    }

    Record DatabaseImpl::getInfo(slice key) const {
        return infoKeyStore().get(key);
    }

    void DatabaseImpl::setInfo(slice key, slice body) {
        infoKeyStore().setKV(key, nullslice, body, transaction());
    }

    void DatabaseImpl::setInfo(Record &rec) {
        infoKeyStore().setKV(rec, transaction());
    }


    KeyStore& DatabaseImpl::rawDocStore(slice storeName) {
        AssertParam(!keyStoreNameToCollectionName(storeName), "Invalid raw-doc store name");
        return _dataFile->getKeyStore(storeName, KeyStore::noSequences);
    }


    bool DatabaseImpl::getRawDocument(slice storeName,
                                      slice key,
                                      function_ref<void(C4RawDocument*)> cb)
    {
        if (Record r = rawDocStore(storeName).get(key); r.exists()) {
            C4RawDocument rawDoc = {r.key(), r.version(), r.body()};
            cb(&rawDoc);
            return true;
        } else {
            cb(nullptr);
            return false;
        }
    }


    void DatabaseImpl::putRawDocument(slice storeName, const C4RawDocument &doc) {
        KeyStore &store = rawDocStore(storeName);
        Transaction t(this);
        if (doc.body.buf || doc.meta.buf)
            store.setKV(doc.key, doc.meta, doc.body, transaction());
        else
            store.del(doc.key, transaction());
        t.commit();
    }


#pragma mark - DOCUMENTS:


    FLSharedKeys DatabaseImpl::getFleeceSharedKeys() const {
        return (FLSharedKeys)_dataFile->documentKeys();
    }


    fleece::impl::Encoder& DatabaseImpl::sharedEncoder() const {
        _encoder->reset();
        return *_encoder.get();
    }


    FLEncoder DatabaseImpl::sharedFleeceEncoder() const {
        if (_flEncoder) {
            FLEncoder_Reset(_flEncoder);
        } else {
            _flEncoder = FLEncoder_NewWithOptions(kFLEncodeFleece, 512, true);
            FLEncoder_SetSharedKeys(_flEncoder, getFleeceSharedKeys());
        }
        return _flEncoder;
    }


    alloc_slice DatabaseImpl::encodeJSON(slice jsonData) const {
        impl::Encoder &enc = sharedEncoder();
        impl::JSONConverter jc(enc);
        if (!jc.encodeJSON(jsonData)) {
            enc.reset();
            error(error::Fleece, jc.errorCode(), jc.errorMessage())._throw();
        }
        return enc.finish();
    }


    FLEncoder DatabaseImpl::createFleeceEncoder() const {
        FLEncoder enc = FLEncoder_NewWithOptions(kFLEncodeFleece, 512, true);
        FLEncoder_SetSharedKeys(enc, getFleeceSharedKeys());
        return enc;
    }


    // Validate that all dictionary keys in this value behave correctly, i.e. the keys found
    // through iteration also work for element lookup. (This tests the fix for issue #156.)
    // In a debug build this scans the entire collection recursively, while release will stick to
    // the top level
    static void validateKeys(const Value *val, bool atRoot =true) {
        // CBL-862: Need to reject invalid top level keys, even in release
        switch (val->type()) {
#if DEBUG
            case kArray:
                for (Array::iterator j(val->asArray()); j; ++j)
                    validateKeys(j.value(), false);
                break;
#endif
            case kDict: {
                const Dict *d = val->asDict();
                for (Dict::iterator i(d); i; ++i) {
                    auto key = i.keyString();
                    if (!key.buf || d->get(key) != i.value())
                        error::_throw(error::CorruptRevisionData,
                                      "Document key is not properly encoded");
                    if (atRoot && (key == "_id"_sl || key == "_rev"_sl || key == "_deleted"_sl))
                        error::_throw(error::CorruptRevisionData,
                                      "Illegal top-level key `%.*s` in document", SPLAT(key));
#if DEBUG
                    if (i.key()->asString() && val->sharedKeys()->couldAdd(key))
                        error::_throw(error::CorruptRevisionData,
                                      "Key `%.*s` should have been shared-key encoded", SPLAT(key));
                    validateKeys(i.value(), false);
#endif
                }
                break;
            }
            default:
                break;
        }
    }


    void DatabaseImpl::validateRevisionBody(slice body) {
        if (body.size > 0) {
            auto documentKeys = _dataFile->documentKeys();
            Scope scope(body, documentKeys);
            const Value *v = Value::fromData(body);
            if (!v)
                error::_throw(error::CorruptRevisionData, "Revision body is not parseable as Fleece");
            const Dict *root = v->asDict();
            if (!root)
                error::_throw(error::CorruptRevisionData, "Revision body is not a Dict");
            if (root->sharedKeys() != documentKeys)
                error::_throw(error::CorruptRevisionData,
                              "Revision uses wrong SharedKeys %p (db's is %p)",
                              root->sharedKeys(), documentKeys);
            validateKeys(v);
        }
    }


#pragma mark - REPLICATION:


    static const char * kRemoteDBURLsDoc = "remotes";


    C4RemoteID DatabaseImpl::getRemoteDBID(slice remoteAddress, bool canCreate) {
        bool inTransaction = false;
        C4RemoteID remoteID = 0;

        // Make two passes: In the first, just look up the "remotes" doc and look for an ID.
        // If the ID isn't found, then do a second pass where we either add the remote URL
        // or create the doc from scratch, in a transaction.
        for (int creating = 0; creating <= 1; ++creating) {
            if (creating) {     // 2nd pass takes place in a transaction
                beginTransaction();
                inTransaction = true;
            }

            // Look up the doc in the db, and the remote URL in the doc:
            Record doc = getInfo(kRemoteDBURLsDoc);
            const impl::Dict *remotes = nullptr;
            remoteID = 0;
            if (doc.exists()) {
                auto body = impl::Value::fromData(doc.body());
                if (body)
                    remotes = body->asDict();
                if (remotes) {
                    auto idObj = remotes->get(remoteAddress);
                    if (idObj)
                        remoteID = C4RemoteID(idObj->asUnsigned());
                }
            }

            if (remoteID > 0) {
                // Found the remote ID!
                return remoteID;
            } else if (!canCreate) {
                break;
            } else if (creating) {
                // Update or create the document, adding the identifier:
                remoteID = 1;
                impl::Encoder enc;
                enc.beginDictionary();
                for (impl::Dict::iterator i(remotes); i; ++i) {
                    auto existingID = i.value()->asUnsigned();
                    if (existingID) {
                        enc.writeKey(i.keyString());            // Copy existing entry
                        enc.writeUInt(existingID);
                        remoteID = max(remoteID, 1 + C4RemoteID(existingID));   // make sure new ID is unique
                    }
                }
                enc.writeKey(remoteAddress);                       // Add new entry
                enc.writeUInt(remoteID);
                enc.endDictionary();
                alloc_slice body = enc.finish();

                // Save the doc:
                setInfo(kRemoteDBURLsDoc, body);
                endTransaction(true);
                inTransaction = false;
                break;
            }
        }
        if (inTransaction)
            endTransaction(false);
        return remoteID;
    }


    alloc_slice DatabaseImpl::getRemoteDBAddress(C4RemoteID remoteID) {
        if (Record doc = getInfo(kRemoteDBURLsDoc); doc.exists()) {
            auto body = impl::Value::fromData(doc.body());
            if (body) {
                for (impl::Dict::iterator i(body->asDict()); i; ++i) {
                    if (i.value()->asInt() == remoteID)
                        return alloc_slice(i.keyString());
                }
            }
        }
        return nullslice;
    }

}
