//
//  Bitstream.cpp
//  libraries/metavoxels/src
//
//  Created by Andrzej Kapolka on 12/2/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <cstring>

#include <QCryptographicHash>
#include <QDataStream>
#include <QMetaType>
#include <QScriptValueIterator>
#include <QUrl>
#include <QtDebug>

#include <RegisteredMetaTypes.h>
#include <SharedUtil.h>

#include "AttributeRegistry.h"
#include "Bitstream.h"
#include "ScriptCache.h"

REGISTER_SIMPLE_TYPE_STREAMER(bool)
REGISTER_SIMPLE_TYPE_STREAMER(int)
REGISTER_SIMPLE_TYPE_STREAMER(uint)
REGISTER_SIMPLE_TYPE_STREAMER(float)
REGISTER_SIMPLE_TYPE_STREAMER(QByteArray)
REGISTER_SIMPLE_TYPE_STREAMER(QColor)
REGISTER_SIMPLE_TYPE_STREAMER(QScriptValue)
REGISTER_SIMPLE_TYPE_STREAMER(QString)
REGISTER_SIMPLE_TYPE_STREAMER(QUrl)
REGISTER_SIMPLE_TYPE_STREAMER(QVariantList)
REGISTER_SIMPLE_TYPE_STREAMER(QVariantHash)
REGISTER_SIMPLE_TYPE_STREAMER(SharedObjectPointer)

// some types don't quite work with our macro
static int vec3Streamer = Bitstream::registerTypeStreamer(qMetaTypeId<glm::vec3>(), new SimpleTypeStreamer<glm::vec3>());
static int quatStreamer = Bitstream::registerTypeStreamer(qMetaTypeId<glm::quat>(), new SimpleTypeStreamer<glm::quat>());
static int metaObjectStreamer = Bitstream::registerTypeStreamer(qMetaTypeId<const QMetaObject*>(),
    new SimpleTypeStreamer<const QMetaObject*>());

static int genericValueStreamer = Bitstream::registerTypeStreamer(
    qRegisterMetaType<GenericValue>(), new GenericTypeStreamer());

IDStreamer::IDStreamer(Bitstream& stream) :
    _stream(stream),
    _bits(1) {
}

static int getBitsForHighestValue(int highestValue) {
    // if this turns out to be a bottleneck, there are fancier ways to do it (get the position of the highest set bit):
    // http://graphics.stanford.edu/~seander/bithacks.html#IntegerLogObvious
    int bits = 0;
    while (highestValue != 0) {
        bits++;
        highestValue >>= 1;
    }
    return bits;
}

void IDStreamer::setBitsFromValue(int value) {
    _bits = getBitsForHighestValue(value + 1);
}

IDStreamer& IDStreamer::operator<<(int value) {
    _stream.write(&value, _bits);
    if (value == (1 << _bits) - 1) {
        _bits++;
    }
    return *this;
}

IDStreamer& IDStreamer::operator>>(int& value) {
    value = 0;
    _stream.read(&value, _bits);
    if (value == (1 << _bits) - 1) {
        _bits++;
    }
    return *this;
}

int Bitstream::registerMetaObject(const char* className, const QMetaObject* metaObject) {
    getMetaObjects().insert(className, metaObject);
    
    // register it as a subclass of itself and all of its superclasses
    for (const QMetaObject* superClass = metaObject; superClass; superClass = superClass->superClass()) {
        getMetaObjectSubClasses().insert(superClass, metaObject);
    }
    return 0;
}

int Bitstream::registerTypeStreamer(int type, TypeStreamer* streamer) {
    streamer->_type = type;
    if (!streamer->_self) {
        streamer->_self = TypeStreamerPointer(streamer);
    }
    getTypeStreamers().insert(type, streamer);
    return 0;
}

const TypeStreamer* Bitstream::getTypeStreamer(int type) {
    return getTypeStreamers().value(type);
}

const QMetaObject* Bitstream::getMetaObject(const QByteArray& className) {
    return getMetaObjects().value(className);
}

QList<const QMetaObject*> Bitstream::getMetaObjectSubClasses(const QMetaObject* metaObject) {
    return getMetaObjectSubClasses().values(metaObject);
}

Bitstream::Bitstream(QDataStream& underlying, MetadataType metadataType, GenericsMode genericsMode, QObject* parent) :
    QObject(parent),
    _underlying(underlying),
    _byte(0),
    _position(0),
    _metadataType(metadataType),
    _genericsMode(genericsMode),
    _metaObjectStreamer(*this),
    _typeStreamerStreamer(*this),
    _attributeStreamer(*this),
    _scriptStringStreamer(*this),
    _sharedObjectStreamer(*this) {
}

void Bitstream::addMetaObjectSubstitution(const QByteArray& className, const QMetaObject* metaObject) {
    _metaObjectSubstitutions.insert(className, metaObject);
}

void Bitstream::addTypeSubstitution(const QByteArray& typeName, int type) {
    _typeStreamerSubstitutions.insert(typeName, getTypeStreamers().value(type));
}

void Bitstream::addTypeSubstitution(const QByteArray& typeName, const char* replacementTypeName) {
    const TypeStreamer* streamer = getTypeStreamers().value(QMetaType::type(replacementTypeName));
    if (!streamer) {
        streamer = getEnumStreamersByName().value(replacementTypeName);
    }
    _typeStreamerSubstitutions.insert(typeName, streamer);
}

const int LAST_BIT_POSITION = BITS_IN_BYTE - 1;

Bitstream& Bitstream::write(const void* data, int bits, int offset) {
    const quint8* source = (const quint8*)data;
    while (bits > 0) {
        int bitsToWrite = qMin(BITS_IN_BYTE - _position, qMin(BITS_IN_BYTE - offset, bits));
        _byte |= ((*source >> offset) & ((1 << bitsToWrite) - 1)) << _position;
        if ((_position += bitsToWrite) == BITS_IN_BYTE) {
            flush();
        }
        if ((offset += bitsToWrite) == BITS_IN_BYTE) {
            source++;
            offset = 0;
        }
        bits -= bitsToWrite;
    }
    return *this;
}

Bitstream& Bitstream::read(void* data, int bits, int offset) {
    quint8* dest = (quint8*)data;
    while (bits > 0) {
        if (_position == 0) {
            _underlying >> _byte;
        }
        int bitsToRead = qMin(BITS_IN_BYTE - _position, qMin(BITS_IN_BYTE - offset, bits));
        int mask = ((1 << bitsToRead) - 1) << offset;
        *dest = (*dest & ~mask) | (((_byte >> _position) << offset) & mask);
        _position = (_position + bitsToRead) & LAST_BIT_POSITION;
        if ((offset += bitsToRead) == BITS_IN_BYTE) {
            dest++;
            offset = 0;
        }
        bits -= bitsToRead;
    }
    return *this;
}

void Bitstream::flush() {
    if (_position != 0) {
        _underlying << _byte;
        reset();
    }
}

void Bitstream::reset() {
    _byte = 0;
    _position = 0;
}

Bitstream::WriteMappings Bitstream::getAndResetWriteMappings() {
    WriteMappings mappings = { _metaObjectStreamer.getAndResetTransientOffsets(),
        _typeStreamerStreamer.getAndResetTransientOffsets(),
        _attributeStreamer.getAndResetTransientOffsets(),
        _scriptStringStreamer.getAndResetTransientOffsets(),
        _sharedObjectStreamer.getAndResetTransientOffsets() };
    return mappings;
}

void Bitstream::persistWriteMappings(const WriteMappings& mappings) {
    _metaObjectStreamer.persistTransientOffsets(mappings.metaObjectOffsets);
    _typeStreamerStreamer.persistTransientOffsets(mappings.typeStreamerOffsets);
    _attributeStreamer.persistTransientOffsets(mappings.attributeOffsets);
    _scriptStringStreamer.persistTransientOffsets(mappings.scriptStringOffsets);
    _sharedObjectStreamer.persistTransientOffsets(mappings.sharedObjectOffsets);
    
    // find out when shared objects are deleted in order to clear their mappings
    for (QHash<SharedObjectPointer, int>::const_iterator it = mappings.sharedObjectOffsets.constBegin();
            it != mappings.sharedObjectOffsets.constEnd(); it++) {
        if (!it.key()) {
            continue;
        }
        connect(it.key().data(), SIGNAL(destroyed(QObject*)), SLOT(clearSharedObject(QObject*)));
        QPointer<SharedObject>& reference = _sharedObjectReferences[it.key()->getOriginID()];
        if (reference && reference != it.key()) {
            // the object has been replaced by a successor, so we can forget about the original
            _sharedObjectStreamer.removePersistentID(reference);
            reference->disconnect(this);
        }
        reference = it.key();
    }
}

void Bitstream::persistAndResetWriteMappings() {
    persistWriteMappings(getAndResetWriteMappings());
}

Bitstream::ReadMappings Bitstream::getAndResetReadMappings() {
    ReadMappings mappings = { _metaObjectStreamer.getAndResetTransientValues(),
        _typeStreamerStreamer.getAndResetTransientValues(),
        _attributeStreamer.getAndResetTransientValues(),
        _scriptStringStreamer.getAndResetTransientValues(),
        _sharedObjectStreamer.getAndResetTransientValues() };
    return mappings;
}

void Bitstream::persistReadMappings(const ReadMappings& mappings) {
    _metaObjectStreamer.persistTransientValues(mappings.metaObjectValues);
    _typeStreamerStreamer.persistTransientValues(mappings.typeStreamerValues);
    _attributeStreamer.persistTransientValues(mappings.attributeValues);
    _scriptStringStreamer.persistTransientValues(mappings.scriptStringValues);
    _sharedObjectStreamer.persistTransientValues(mappings.sharedObjectValues);
    
    for (QHash<int, SharedObjectPointer>::const_iterator it = mappings.sharedObjectValues.constBegin();
            it != mappings.sharedObjectValues.constEnd(); it++) {
        if (!it.value()) {
            continue;
        }
        QPointer<SharedObject>& reference = _sharedObjectReferences[it.value()->getRemoteOriginID()];
        if (reference && reference != it.value()) {
            // the object has been replaced by a successor, so we can forget about the original
            _sharedObjectStreamer.removePersistentValue(reference.data());
        }
        reference = it.value();
        _weakSharedObjectHash.remove(it.value()->getRemoteID());
    }
}

void Bitstream::persistAndResetReadMappings() {
    persistReadMappings(getAndResetReadMappings());
}

void Bitstream::clearSharedObject(int id) {
    SharedObjectPointer object = _sharedObjectStreamer.takePersistentValue(id);
    if (object) {
        _weakSharedObjectHash.remove(object->getRemoteID());
    }
}

void Bitstream::writeDelta(bool value, bool reference) {
    *this << value;
}

void Bitstream::readDelta(bool& value, bool reference) {
    *this >> value;
}

void Bitstream::writeDelta(const QVariant& value, const QVariant& reference) {
    // QVariant only handles == for built-in types; we need to use our custom operators
    const TypeStreamer* streamer = getTypeStreamers().value(value.userType());
    if (value.userType() == reference.userType() && (!streamer || streamer->equal(value, reference))) {
        *this << false;
         return;
    }
    *this << true;
    _typeStreamerStreamer << streamer;
    streamer->writeRawDelta(*this, value, reference);
}

void Bitstream::writeRawDelta(const QVariant& value, const QVariant& reference) {
    const TypeStreamer* streamer = getTypeStreamers().value(value.userType());
    _typeStreamerStreamer << streamer;
    streamer->writeRawDelta(*this, value, reference);
}

void Bitstream::readRawDelta(QVariant& value, const QVariant& reference) {
    TypeReader typeReader;
    _typeStreamerStreamer >> typeReader;
    typeReader.readRawDelta(*this, value, reference);
}

void Bitstream::writeRawDelta(const QObject* value, const QObject* reference) {
    if (!value) {
        _metaObjectStreamer << NULL;
        return;
    }
    const QMetaObject* metaObject = value->metaObject();
    _metaObjectStreamer << metaObject;
    foreach (const PropertyWriter& propertyWriter, getPropertyWriters().value(metaObject)) {
        propertyWriter.writeDelta(*this, value, reference);
    }
}

void Bitstream::readRawDelta(QObject*& value, const QObject* reference) {
    ObjectReader objectReader;
    _metaObjectStreamer >> objectReader;
    value = objectReader.readDelta(*this, reference);
}

void Bitstream::writeRawDelta(const QScriptValue& value, const QScriptValue& reference) {
    if (reference.isUndefined() || reference.isNull()) {
        *this << value;
    
    } else if (reference.isBool()) {
        if (value.isBool()) {
            *this << false;
            *this << value.toBool();
            
        } else {
            *this << true;
            *this << value;
        }
    } else if (reference.isNumber()) {
        if (value.isNumber()) {
            *this << false;
            *this << value.toNumber();
            
        } else {
            *this << true;
            *this << value;
        }
    } else if (reference.isString()) {
        if (value.isString()) {
            *this << false;
            *this << value.toString();
            
        } else {
            *this << true;
            *this << value;
        }
    } else if (reference.isVariant()) {
        if (value.isVariant()) {
            *this << false;
            writeRawDelta(value.toVariant(), reference.toVariant());
            
        } else {
            *this << true;
            *this << value;
        }
    } else if (reference.isQObject()) {
        if (value.isQObject()) {
            *this << false;
            writeRawDelta(value.toQObject(), reference.toQObject());
            
        } else {
            *this << true;
            *this << value;
        }
    } else if (reference.isQMetaObject()) {
        if (value.isQMetaObject()) {
            *this << false;
            *this << value.toQMetaObject();
            
        } else {
            *this << true;
            *this << value;
        }
    } else if (reference.isDate()) {
        if (value.isDate()) {
            *this << false;
            *this << value.toDateTime();
            
        } else {
            *this << true;
            *this << value;
        }
    } else if (reference.isRegExp()) {
        if (value.isRegExp()) {
            *this << false;
            *this << value.toRegExp();
            
        } else {
            *this << true;
            *this << value;
        }
    } else if (reference.isArray()) {
        if (value.isArray()) {
            *this << false;
            int length = value.property(ScriptCache::getInstance()->getLengthString()).toInt32();
            *this << length;
            int referenceLength = reference.property(ScriptCache::getInstance()->getLengthString()).toInt32();
            for (int i = 0; i < length; i++) {
                if (i < referenceLength) {
                    writeDelta(value.property(i), reference.property(i));
                } else {
                    *this << value.property(i);
                }
            }
        } else {
            *this << true;
            *this << value;
        }
    } else if (reference.isObject()) {
        if (value.isObject() && !(value.isArray() || value.isRegExp() || value.isDate() ||
                value.isQMetaObject() || value.isQObject() || value.isVariant())) {    
            *this << false;
            for (QScriptValueIterator it(value); it.hasNext(); ) {
                it.next();
                QScriptValue referenceValue = reference.property(it.scriptName());
                if (it.value() != referenceValue) {
                    *this << it.scriptName();
                    writeRawDelta(it.value(), referenceValue);
                }
            }
            for (QScriptValueIterator it(reference); it.hasNext(); ) {
                it.next();
                if (!value.property(it.scriptName()).isValid()) {
                    *this << it.scriptName();
                    writeRawDelta(QScriptValue(), it.value());
                }
            }
            *this << QScriptString();
            
        } else {
            *this << true;
            *this << value;
        }
    } else {
        *this << value;
    }
}

void Bitstream::readRawDelta(QScriptValue& value, const QScriptValue& reference) {
    if (reference.isUndefined() || reference.isNull()) {
        *this >> value;
    
    } else if (reference.isBool()) {
        bool typeChanged;
        *this >> typeChanged;
        if (typeChanged) {
            *this >> value;
            
        } else {
            bool boolValue;
            *this >> boolValue;
            value = QScriptValue(boolValue);
        }
    } else if (reference.isNumber()) {
        bool typeChanged;
        *this >> typeChanged;
        if (typeChanged) {
            *this >> value;
            
        } else {
            qsreal numberValue;
            *this >> numberValue;
            value = QScriptValue(numberValue);
        }
    } else if (reference.isString()) {
        bool typeChanged;
        *this >> typeChanged;
        if (typeChanged) {
            *this >> value;
            
        } else {
            QString stringValue;
            *this >> stringValue;
            value = QScriptValue(stringValue);
        }
    } else if (reference.isVariant()) {
        bool typeChanged;
        *this >> typeChanged;
        if (typeChanged) {
            *this >> value;
            
        } else {
            QVariant variant;
            readRawDelta(variant, reference.toVariant());
            value = ScriptCache::getInstance()->getEngine()->newVariant(variant);
        }
    } else if (reference.isQObject()) {
        bool typeChanged;
        *this >> typeChanged;
        if (typeChanged) {
            *this >> value;
            
        } else {
            QObject* object;
            readRawDelta(object, reference.toQObject());
            value = ScriptCache::getInstance()->getEngine()->newQObject(object, QScriptEngine::ScriptOwnership);
        }
    } else if (reference.isQMetaObject()) {
        bool typeChanged;
        *this >> typeChanged;
        if (typeChanged) {
            *this >> value;
            
        } else {
            const QMetaObject* metaObject;
            *this >> metaObject;
            value = ScriptCache::getInstance()->getEngine()->newQMetaObject(metaObject);
        }
    } else if (reference.isDate()) {
        bool typeChanged;
        *this >> typeChanged;
        if (typeChanged) {
            *this >> value;
            
        } else {
            QDateTime dateTime;
            *this >> dateTime;
            value = ScriptCache::getInstance()->getEngine()->newDate(dateTime);
        }
    } else if (reference.isRegExp()) {
        bool typeChanged;
        *this >> typeChanged;
        if (typeChanged) {
            *this >> value;
            
        } else {
            QRegExp regExp;
            *this >> regExp;
            value = ScriptCache::getInstance()->getEngine()->newRegExp(regExp);
        }
    } else if (reference.isArray()) {
        bool typeChanged;
        *this >> typeChanged;
        if (typeChanged) {
            *this >> value;
            
        } else {
            int length;
            *this >> length;
            value = ScriptCache::getInstance()->getEngine()->newArray(length);
            int referenceLength = reference.property(ScriptCache::getInstance()->getLengthString()).toInt32();
            for (int i = 0; i < length; i++) {
                QScriptValue element;
                if (i < referenceLength) {
                    readDelta(element, reference.property(i));
                } else {
                    *this >> element;
                }
                value.setProperty(i, element);
            }
        }
    } else if (reference.isObject()) {
        bool typeChanged;
        *this >> typeChanged;
        if (typeChanged) {
            *this >> value;
            
        } else {
            // start by shallow-copying the reference
            value = ScriptCache::getInstance()->getEngine()->newObject();
            for (QScriptValueIterator it(reference); it.hasNext(); ) {
                it.next();
                value.setProperty(it.scriptName(), it.value());
            }
            // then apply the requested changes
            forever {
                QScriptString name;
                *this >> name;
                if (!name.isValid()) {
                    break;
                }
                QScriptValue scriptValue;
                readRawDelta(scriptValue, reference.property(name));
                value.setProperty(name, scriptValue);
            }
        }
    } else {
        *this >> value;
    }
}

Bitstream& Bitstream::operator<<(bool value) {
    if (value) {
        _byte |= (1 << _position);
    }
    if (++_position == BITS_IN_BYTE) {
        flush();
    }
    return *this;
}

Bitstream& Bitstream::operator>>(bool& value) {
    if (_position == 0) {
        _underlying >> _byte;
    }
    value = _byte & (1 << _position);
    _position = (_position + 1) & LAST_BIT_POSITION;
    return *this;
}

Bitstream& Bitstream::operator<<(int value) {
    return write(&value, 32);
}

Bitstream& Bitstream::operator>>(int& value) {
    qint32 sizedValue;
    read(&sizedValue, 32);
    value = sizedValue;
    return *this;
}

Bitstream& Bitstream::operator<<(uint value) {
    return write(&value, 32);
}

Bitstream& Bitstream::operator>>(uint& value) {
    quint32 sizedValue;
    read(&sizedValue, 32);
    value = sizedValue;
    return *this;
}

Bitstream& Bitstream::operator<<(qint64 value) {
    return write(&value, 64);
}

Bitstream& Bitstream::operator>>(qint64& value) {
    return read(&value, 64);
}

Bitstream& Bitstream::operator<<(float value) {
    return write(&value, 32);
}

Bitstream& Bitstream::operator>>(float& value) {
    return read(&value, 32);
}

Bitstream& Bitstream::operator<<(double value) {
    return write(&value, 64);
}

Bitstream& Bitstream::operator>>(double& value) {
    return read(&value, 64);
}

Bitstream& Bitstream::operator<<(const glm::vec3& value) {
    return *this << value.x << value.y << value.z;
}

Bitstream& Bitstream::operator>>(glm::vec3& value) {
    return *this >> value.x >> value.y >> value.z;
}

Bitstream& Bitstream::operator<<(const glm::quat& value) {
    return *this << value.w << value.x << value.y << value.z;
}

Bitstream& Bitstream::operator>>(glm::quat& value) {
    return *this >> value.w >> value.x >> value.y >> value.z;
}

Bitstream& Bitstream::operator<<(const QByteArray& string) {
    *this << string.size();
    return write(string.constData(), string.size() * BITS_IN_BYTE);
}

Bitstream& Bitstream::operator>>(QByteArray& string) {
    int size;
    *this >> size;
    string.resize(size);
    return read(string.data(), size * BITS_IN_BYTE);
}

Bitstream& Bitstream::operator<<(const QColor& color) {
    return *this << (int)color.rgba();
}

Bitstream& Bitstream::operator>>(QColor& color) {
    int rgba;
    *this >> rgba;
    color.setRgba(rgba);
    return *this;
}

Bitstream& Bitstream::operator<<(const QString& string) {
    *this << string.size();    
    return write(string.constData(), string.size() * sizeof(QChar) * BITS_IN_BYTE);
}

Bitstream& Bitstream::operator>>(QString& string) {
    int size;
    *this >> size;
    string.resize(size);
    return read(string.data(), size * sizeof(QChar) * BITS_IN_BYTE);
}

Bitstream& Bitstream::operator<<(const QUrl& url) {
    return *this << url.toString();
}

Bitstream& Bitstream::operator>>(QUrl& url) {
    QString string;
    *this >> string;
    url = string;
    return *this;
}

Bitstream& Bitstream::operator<<(const QDateTime& dateTime) {
    return *this << dateTime.toMSecsSinceEpoch();
}

Bitstream& Bitstream::operator>>(QDateTime& dateTime) {
    qint64 msecsSinceEpoch;
    *this >> msecsSinceEpoch;
    dateTime = QDateTime::fromMSecsSinceEpoch(msecsSinceEpoch);
    return *this;
}

Bitstream& Bitstream::operator<<(const QRegExp& regExp) {
    *this << regExp.pattern();
    Qt::CaseSensitivity caseSensitivity = regExp.caseSensitivity();
    write(&caseSensitivity, 1);
    QRegExp::PatternSyntax syntax = regExp.patternSyntax();
    write(&syntax, 3);
    return *this << regExp.isMinimal();
}

Bitstream& Bitstream::operator>>(QRegExp& regExp) {
    QString pattern;
    *this >> pattern;
    Qt::CaseSensitivity caseSensitivity = (Qt::CaseSensitivity)0;
    read(&caseSensitivity, 1);
    QRegExp::PatternSyntax syntax = (QRegExp::PatternSyntax)0;
    read(&syntax, 3);
    regExp = QRegExp(pattern, caseSensitivity, syntax);
    bool minimal;
    *this >> minimal;
    regExp.setMinimal(minimal);
    return *this;
}

Bitstream& Bitstream::operator<<(const QVariant& value) {
    if (!value.isValid()) {
        _typeStreamerStreamer << NULL;
        return *this;
    }
    const TypeStreamer* streamer = getTypeStreamers().value(value.userType());
    if (streamer) {
        _typeStreamerStreamer << streamer->getStreamerToWrite(value);
        streamer->write(*this, value);
    } else {
        qWarning() << "Non-streamable type: " << value.typeName() << "\n";
    }
    return *this;
}

Bitstream& Bitstream::operator>>(QVariant& value) {
    TypeReader reader;
    _typeStreamerStreamer >> reader;
    if (reader.getTypeName().isEmpty()) {
        value = QVariant();
    } else {
        value = reader.read(*this);
    }
    return *this;
}

Bitstream& Bitstream::operator<<(const AttributeValue& attributeValue) {
    _attributeStreamer << attributeValue.getAttribute();
    if (attributeValue.getAttribute()) {
        attributeValue.getAttribute()->write(*this, attributeValue.getValue(), true);
    }
    return *this;
}

Bitstream& Bitstream::operator>>(OwnedAttributeValue& attributeValue) {
    AttributePointer attribute;
    _attributeStreamer >> attribute;
    if (attribute) {
        void* value = attribute->create();
        attribute->read(*this, value, true);
        attributeValue = AttributeValue(attribute, value);
        attribute->destroy(value);
        
    } else {
        attributeValue = AttributeValue();
    }
    return *this;
}

Bitstream& Bitstream::operator<<(const GenericValue& value) {
    value.getStreamer()->write(*this, value.getValue());
    return *this;
}

Bitstream& Bitstream::operator>>(GenericValue& value) {
    value = GenericValue();
    return *this;
}

Bitstream& Bitstream::operator<<(const QObject* object) {
    if (!object) {
        _metaObjectStreamer << NULL;
        return *this;
    }
    const QMetaObject* metaObject = object->metaObject();
    _metaObjectStreamer << metaObject;
    foreach (const PropertyWriter& propertyWriter, getPropertyWriters().value(metaObject)) {
        propertyWriter.write(*this, object);
    }
    return *this;
}

Bitstream& Bitstream::operator>>(QObject*& object) {
    ObjectReader objectReader;
    _metaObjectStreamer >> objectReader;
    object = objectReader.read(*this);
    return *this;
}

Bitstream& Bitstream::operator<<(const QMetaObject* metaObject) {
    _metaObjectStreamer << metaObject;
    return *this;
}

Bitstream& Bitstream::operator>>(const QMetaObject*& metaObject) {
    ObjectReader objectReader;
    _metaObjectStreamer >> objectReader;
    metaObject = objectReader.getMetaObject();
    return *this;
}

Bitstream& Bitstream::operator>>(ObjectReader& objectReader) {
    _metaObjectStreamer >> objectReader;
    return *this;
}

Bitstream& Bitstream::operator<<(const TypeStreamer* streamer) {
    _typeStreamerStreamer << streamer;    
    return *this;
}

Bitstream& Bitstream::operator>>(const TypeStreamer*& streamer) {
    TypeReader typeReader;
    _typeStreamerStreamer >> typeReader;
    streamer = typeReader.getStreamer();
    return *this;
}

Bitstream& Bitstream::operator>>(TypeReader& reader) {
    _typeStreamerStreamer >> reader;
    return *this;
}

Bitstream& Bitstream::operator<<(const AttributePointer& attribute) {
    _attributeStreamer << attribute;
    return *this;
}

Bitstream& Bitstream::operator>>(AttributePointer& attribute) {
    _attributeStreamer >> attribute;
    return *this;
}

Bitstream& Bitstream::operator<<(const QScriptString& string) {
    _scriptStringStreamer << string;
    return *this;
}

Bitstream& Bitstream::operator>>(QScriptString& string) {
    _scriptStringStreamer >> string;
    return *this;
}

enum ScriptValueType {
    INVALID_SCRIPT_VALUE,
    UNDEFINED_SCRIPT_VALUE,
    NULL_SCRIPT_VALUE,
    BOOL_SCRIPT_VALUE,
    NUMBER_SCRIPT_VALUE,
    STRING_SCRIPT_VALUE,
    VARIANT_SCRIPT_VALUE,
    QOBJECT_SCRIPT_VALUE,
    QMETAOBJECT_SCRIPT_VALUE,
    DATE_SCRIPT_VALUE,
    REGEXP_SCRIPT_VALUE,
    ARRAY_SCRIPT_VALUE,
    OBJECT_SCRIPT_VALUE
};

const int SCRIPT_VALUE_BITS = 4;

static void writeScriptValueType(Bitstream& out, ScriptValueType type) {
    out.write(&type, SCRIPT_VALUE_BITS);
}

static ScriptValueType readScriptValueType(Bitstream& in) {
    ScriptValueType type = (ScriptValueType)0;
    in.read(&type, SCRIPT_VALUE_BITS);
    return type;
}

Bitstream& Bitstream::operator<<(const QScriptValue& value) {
    if (value.isUndefined()) {
        writeScriptValueType(*this, UNDEFINED_SCRIPT_VALUE);
        
    } else if (value.isNull()) {
        writeScriptValueType(*this, NULL_SCRIPT_VALUE);
    
    } else if (value.isBool()) {
        writeScriptValueType(*this, BOOL_SCRIPT_VALUE);
        *this << value.toBool();
    
    } else if (value.isNumber()) {
        writeScriptValueType(*this, NUMBER_SCRIPT_VALUE);
        *this << value.toNumber();
    
    } else if (value.isString()) {
        writeScriptValueType(*this, STRING_SCRIPT_VALUE);
        *this << value.toString();
    
    } else if (value.isVariant()) {
        writeScriptValueType(*this, VARIANT_SCRIPT_VALUE);
        *this << value.toVariant();
        
    } else if (value.isQObject()) {
        writeScriptValueType(*this, QOBJECT_SCRIPT_VALUE);
        *this << value.toQObject();
    
    } else if (value.isQMetaObject()) {
        writeScriptValueType(*this, QMETAOBJECT_SCRIPT_VALUE);
        *this << value.toQMetaObject();
        
    } else if (value.isDate()) {
        writeScriptValueType(*this, DATE_SCRIPT_VALUE);
        *this << value.toDateTime();
    
    } else if (value.isRegExp()) {
        writeScriptValueType(*this, REGEXP_SCRIPT_VALUE);
        *this << value.toRegExp();
    
    } else if (value.isArray()) {
        writeScriptValueType(*this, ARRAY_SCRIPT_VALUE);
        int length = value.property(ScriptCache::getInstance()->getLengthString()).toInt32();
        *this << length;
        for (int i = 0; i < length; i++) {
            *this << value.property(i);
        }
    } else if (value.isObject()) {
        writeScriptValueType(*this, OBJECT_SCRIPT_VALUE);
        for (QScriptValueIterator it(value); it.hasNext(); ) {
            it.next();
            *this << it.scriptName();
            *this << it.value();
        }
        *this << QScriptString();
        
    } else {
        writeScriptValueType(*this, INVALID_SCRIPT_VALUE);
    }
    return *this;
}

Bitstream& Bitstream::operator>>(QScriptValue& value) {
    switch (readScriptValueType(*this)) {
        case UNDEFINED_SCRIPT_VALUE:
            value = QScriptValue(QScriptValue::UndefinedValue);
            break;
        
        case NULL_SCRIPT_VALUE:
            value = QScriptValue(QScriptValue::NullValue);
            break;
        
        case BOOL_SCRIPT_VALUE: {
            bool boolValue;
            *this >> boolValue;
            value = QScriptValue(boolValue);
            break;
        }
        case NUMBER_SCRIPT_VALUE: {
            qsreal numberValue;
            *this >> numberValue;
            value = QScriptValue(numberValue);
            break;
        }
        case STRING_SCRIPT_VALUE: {
            QString stringValue;
            *this >> stringValue;   
            value = QScriptValue(stringValue);
            break;
        }
        case VARIANT_SCRIPT_VALUE: {
            QVariant variantValue;
            *this >> variantValue;
            value = ScriptCache::getInstance()->getEngine()->newVariant(variantValue);
            break;
        }
        case QOBJECT_SCRIPT_VALUE: {
            QObject* object;
            *this >> object;
            ScriptCache::getInstance()->getEngine()->newQObject(object, QScriptEngine::ScriptOwnership);
            break;
        }
        case QMETAOBJECT_SCRIPT_VALUE: {
            const QMetaObject* metaObject;
            *this >> metaObject;
            ScriptCache::getInstance()->getEngine()->newQMetaObject(metaObject);
            break;
        }
        case DATE_SCRIPT_VALUE: {
            QDateTime dateTime;
            *this >> dateTime;
            value = ScriptCache::getInstance()->getEngine()->newDate(dateTime);
            break;
        }
        case REGEXP_SCRIPT_VALUE: {
            QRegExp regExp;
            *this >> regExp;
            value = ScriptCache::getInstance()->getEngine()->newRegExp(regExp);
            break;
        }
        case ARRAY_SCRIPT_VALUE: {
            int length;
            *this >> length;
            value = ScriptCache::getInstance()->getEngine()->newArray(length);
            for (int i = 0; i < length; i++) {
                QScriptValue element;
                *this >> element;
                value.setProperty(i, element);
            }
            break;
        }
        case OBJECT_SCRIPT_VALUE: {
            value = ScriptCache::getInstance()->getEngine()->newObject();
            forever {
                QScriptString name;
                *this >> name;
                if (!name.isValid()) {
                    break;
                }
                QScriptValue scriptValue;
                *this >> scriptValue;
                value.setProperty(name, scriptValue);
            }
            break;
        }
        default:
            value = QScriptValue();
            break;
    }
    return *this;
}

Bitstream& Bitstream::operator<<(const SharedObjectPointer& object) {
    _sharedObjectStreamer << object;
    return *this;
}

Bitstream& Bitstream::operator>>(SharedObjectPointer& object) {
    _sharedObjectStreamer >> object;
    return *this;
}

Bitstream& Bitstream::operator<(const QMetaObject* metaObject) {
    if (!metaObject) {
        return *this << QByteArray();
    }
    *this << QByteArray::fromRawData(metaObject->className(), strlen(metaObject->className()));
    if (_metadataType == NO_METADATA) {
        return *this;
    }
    const PropertyWriterVector& propertyWriters = getPropertyWriters().value(metaObject);
    *this << propertyWriters.size();
    QCryptographicHash hash(QCryptographicHash::Md5);
    foreach (const PropertyWriter& propertyWriter, propertyWriters) {
        _typeStreamerStreamer << propertyWriter.getStreamer();
        const QMetaProperty& property = propertyWriter.getProperty();
        if (_metadataType == FULL_METADATA) {
            *this << QByteArray::fromRawData(property.name(), strlen(property.name()));
        } else {
            hash.addData(property.name(), strlen(property.name()) + 1);
        }
    }
    if (_metadataType == HASH_METADATA) {
        QByteArray hashResult = hash.result();
        write(hashResult.constData(), hashResult.size() * BITS_IN_BYTE);
    }
    return *this;
}

Bitstream& Bitstream::operator>(ObjectReader& objectReader) {
    QByteArray className;
    *this >> className;
    if (className.isEmpty()) {
        objectReader = ObjectReader();
        return *this;
    }
    const QMetaObject* metaObject = _metaObjectSubstitutions.value(className);
    if (!metaObject) {
        metaObject = getMetaObjects().value(className);
    }
    if (!metaObject) {
        qWarning() << "Unknown class name: " << className << "\n";
    }
    if (_metadataType == NO_METADATA) {
        objectReader = ObjectReader(className, metaObject, getPropertyReaders().value(metaObject));
        return *this;
    }
    int storedPropertyCount;
    *this >> storedPropertyCount;
    PropertyReaderVector properties(storedPropertyCount);
    for (int i = 0; i < storedPropertyCount; i++) {
        TypeReader typeReader;
        *this >> typeReader;
        QMetaProperty property = QMetaProperty();
        if (_metadataType == FULL_METADATA) {
            QByteArray propertyName;
            *this >> propertyName;
            if (metaObject) {
                property = metaObject->property(metaObject->indexOfProperty(propertyName));
            }
        }
        properties[i] = PropertyReader(typeReader, property);
    }
    // for hash metadata, check the names/types of the properties as well as the name hash against our own class
    if (_metadataType == HASH_METADATA) {
        QCryptographicHash hash(QCryptographicHash::Md5);
        bool matches = true;
        if (metaObject) {
            const PropertyWriterVector& propertyWriters = getPropertyWriters().value(metaObject);
            if (propertyWriters.size() == properties.size()) {
                for (int i = 0; i < propertyWriters.size(); i++) {
                    const PropertyWriter& propertyWriter = propertyWriters.at(i);
                    if (!properties.at(i).getReader().matchesExactly(propertyWriter.getStreamer())) {
                        matches = false;
                        break;
                    }
                    const QMetaProperty& property = propertyWriter.getProperty();
                    hash.addData(property.name(), strlen(property.name()) + 1); 
                }
            } else {
                matches = false;
            }
        }
        QByteArray localHashResult = hash.result();
        QByteArray remoteHashResult(localHashResult.size(), 0);
        read(remoteHashResult.data(), remoteHashResult.size() * BITS_IN_BYTE);
        if (metaObject && matches && localHashResult == remoteHashResult) {
            objectReader = ObjectReader(className, metaObject, getPropertyReaders().value(metaObject));
            return *this;
        }
    }
    objectReader = ObjectReader(className, metaObject, properties);
    return *this;
}

Bitstream& Bitstream::operator<(const TypeStreamer* streamer) {
    if (!streamer) {
        *this << QByteArray();
        return *this;
    }
    const char* typeName = streamer->getName();
    *this << QByteArray::fromRawData(typeName, strlen(typeName));
    if (_metadataType == NO_METADATA) {
        return *this;
    }
    TypeReader::Type type = streamer->getReaderType();
    *this << (int)type;
    switch (type) {
        case TypeReader::SIMPLE_TYPE:
            return *this;
        
        case TypeReader::ENUM_TYPE: {
            QMetaEnum metaEnum = streamer->getMetaEnum();
            if (_metadataType == FULL_METADATA) {
                *this << metaEnum.keyCount();
                for (int i = 0; i < metaEnum.keyCount(); i++) {
                    *this << QByteArray::fromRawData(metaEnum.key(i), strlen(metaEnum.key(i)));
                    *this << metaEnum.value(i);
                }
            } else {
                *this << streamer->getBits();
                QCryptographicHash hash(QCryptographicHash::Md5);    
                for (int i = 0; i < metaEnum.keyCount(); i++) {
                    hash.addData(metaEnum.key(i), strlen(metaEnum.key(i)) + 1);
                    qint32 value = metaEnum.value(i);
                    hash.addData((const char*)&value, sizeof(qint32));
                }
                QByteArray hashResult = hash.result();
                write(hashResult.constData(), hashResult.size() * BITS_IN_BYTE);
            }
            return *this;
        }
        case TypeReader::LIST_TYPE:
        case TypeReader::SET_TYPE:
            return *this << streamer->getValueStreamer();
    
        case TypeReader::MAP_TYPE:
            return *this << streamer->getKeyStreamer() << streamer->getValueStreamer();
        
        default:
            break; // fall through
    }
    // streamable type
    const QVector<MetaField>& metaFields = streamer->getMetaFields();
    *this << metaFields.size();
    if (metaFields.isEmpty()) {
        return *this;
    }
    QCryptographicHash hash(QCryptographicHash::Md5);
    foreach (const MetaField& metaField, metaFields) {
        _typeStreamerStreamer << metaField.getStreamer();
        if (_metadataType == FULL_METADATA) {
            *this << metaField.getName();
        } else {
            hash.addData(metaField.getName().constData(), metaField.getName().size() + 1);
        }
    }
    if (_metadataType == HASH_METADATA) {
        QByteArray hashResult = hash.result();
        write(hashResult.constData(), hashResult.size() * BITS_IN_BYTE);
    }
    return *this;
}

Bitstream& Bitstream::operator>(TypeReader& reader) {
    QByteArray typeName;
    *this >> typeName;
    if (typeName.isEmpty()) {
        reader = TypeReader();
        return *this;
    }
    const TypeStreamer* streamer = _typeStreamerSubstitutions.value(typeName);
    if (!streamer) {
        streamer = getTypeStreamers().value(QMetaType::type(typeName.constData()));
        if (!streamer) {
            streamer = getEnumStreamersByName().value(typeName);
        }
    }
    if (_metadataType == NO_METADATA) {
        if (!streamer) {
            qWarning() << "Unknown type name:" << typeName;
        }
        reader = TypeReader(typeName, streamer);
        return *this;
    }
    int type;
    *this >> type;
    if (type == TypeReader::SIMPLE_TYPE) {
        if (!streamer) {
            qWarning() << "Unknown type name:" << typeName;
        }
        reader = TypeReader(typeName, streamer);
        return *this;
    }
    if (_genericsMode == ALL_GENERICS) {
        streamer = NULL;
    }
    switch (type) {
        case TypeReader::ENUM_TYPE: {
            if (_metadataType == FULL_METADATA) {
                int keyCount;
                *this >> keyCount;
                QMetaEnum metaEnum = (streamer && streamer->getReaderType() == TypeReader::ENUM_TYPE) ?
                    streamer->getMetaEnum() : QMetaEnum();
                QHash<int, int> mappings;
                bool matches = (keyCount == metaEnum.keyCount());
                int highestValue = 0;
                for (int i = 0; i < keyCount; i++) {
                    QByteArray key;
                    int value;
                    *this >> key >> value;
                    highestValue = qMax(value, highestValue);
                    int localValue = metaEnum.keyToValue(key);
                    if (localValue != -1) {
                        mappings.insert(value, localValue);
                    }
                    matches &= (value == localValue);
                }
                if (matches) {
                    reader = TypeReader(typeName, streamer);
                } else {
                    reader = TypeReader(typeName, streamer, getBitsForHighestValue(highestValue), mappings);
                }
            } else {
                int bits;
                *this >> bits;
                QCryptographicHash hash(QCryptographicHash::Md5);
                if (streamer && streamer->getReaderType() == TypeReader::ENUM_TYPE) {
                    QMetaEnum metaEnum = streamer->getMetaEnum();
                    for (int i = 0; i < metaEnum.keyCount(); i++) {
                        hash.addData(metaEnum.key(i), strlen(metaEnum.key(i)) + 1);
                        qint32 value = metaEnum.value(i);
                        hash.addData((const char*)&value, sizeof(qint32));
                    }
                }
                QByteArray localHashResult = hash.result();
                QByteArray remoteHashResult(localHashResult.size(), 0);
                read(remoteHashResult.data(), remoteHashResult.size() * BITS_IN_BYTE);
                if (localHashResult == remoteHashResult) {
                    reader = TypeReader(typeName, streamer);
                } else {
                    reader = TypeReader(typeName, streamer, bits, QHash<int, int>());
                }
            }
            return *this;
        }
        case TypeReader::LIST_TYPE:
        case TypeReader::SET_TYPE: {
            TypeReader valueReader;
            *this >> valueReader;
            if (streamer && streamer->getReaderType() == type &&
                    valueReader.matchesExactly(streamer->getValueStreamer())) {
                reader = TypeReader(typeName, streamer);
            } else {
                reader = TypeReader(typeName, streamer, (TypeReader::Type)type,
                    TypeReaderPointer(new TypeReader(valueReader)));
            }
            return *this;
        }
        case TypeReader::MAP_TYPE: {
            TypeReader keyReader, valueReader;
            *this >> keyReader >> valueReader;
            if (streamer && streamer->getReaderType() == TypeReader::MAP_TYPE &&
                    keyReader.matchesExactly(streamer->getKeyStreamer()) &&
                    valueReader.matchesExactly(streamer->getValueStreamer())) {
                reader = TypeReader(typeName, streamer);
            } else {
                reader = TypeReader(typeName, streamer, TypeReaderPointer(new TypeReader(keyReader)),
                    TypeReaderPointer(new TypeReader(valueReader)));
            }
            return *this;
        }
    }
    // streamable type
    int fieldCount;
    *this >> fieldCount;
    QVector<FieldReader> fields(fieldCount);
    for (int i = 0; i < fieldCount; i++) {
        TypeReader typeReader;
        *this >> typeReader;
        int index = -1;
        if (_metadataType == FULL_METADATA) {
            QByteArray fieldName;
            *this >> fieldName;
            if (streamer) {
                index = streamer->getFieldIndex(fieldName);
            }
        }
        fields[i] = FieldReader(typeReader, index);
    }
    // for hash metadata, check the names/types of the fields as well as the name hash against our own class
    if (_metadataType == HASH_METADATA) {
        QCryptographicHash hash(QCryptographicHash::Md5);
        bool matches = true;
        if (streamer) {
            const QVector<MetaField>& localFields = streamer->getMetaFields();
            if (fieldCount != localFields.size()) {
                matches = false;
                
            } else {
                if (fieldCount == 0) {
                    reader = TypeReader(typeName, streamer);
                    return *this;
                }
                for (int i = 0; i < fieldCount; i++) {
                    const MetaField& localField = localFields.at(i);
                    if (!fields.at(i).getReader().matchesExactly(localField.getStreamer())) {
                        matches = false;
                        break;
                    }
                    hash.addData(localField.getName().constData(), localField.getName().size() + 1);
                }   
            }
        }
        QByteArray localHashResult = hash.result();
        QByteArray remoteHashResult(localHashResult.size(), 0);
        read(remoteHashResult.data(), remoteHashResult.size() * BITS_IN_BYTE);
        if (streamer && matches && localHashResult == remoteHashResult) {
            // since everything is the same, we can use the default streamer
            reader = TypeReader(typeName, streamer);
            return *this;
        }
    } else if (streamer) {
        // if all fields are the same type and in the right order, we can use the (more efficient) default streamer
        const QVector<MetaField>& localFields = streamer->getMetaFields();
        if (fieldCount != localFields.size()) {
            reader = TypeReader(typeName, streamer, fields);
            return *this;
        }
        for (int i = 0; i < fieldCount; i++) {
            const FieldReader& fieldReader = fields.at(i);
            if (!fieldReader.getReader().matchesExactly(localFields.at(i).getStreamer()) || fieldReader.getIndex() != i) {
                reader = TypeReader(typeName, streamer, fields);
                return *this;
            }
        }
        reader = TypeReader(typeName, streamer);
        return *this;
    }
    reader = TypeReader(typeName, streamer, fields);
    return *this;
}

Bitstream& Bitstream::operator<(const AttributePointer& attribute) {
    return *this << (QObject*)attribute.data();
}

Bitstream& Bitstream::operator>(AttributePointer& attribute) {
    QObject* object;
    *this >> object;
    attribute = AttributeRegistry::getInstance()->registerAttribute(static_cast<Attribute*>(object));
    return *this;
}

const QString INVALID_STRING("%INVALID%");

Bitstream& Bitstream::operator<(const QScriptString& string) {
    return *this << (string.isValid() ? string.toString() : INVALID_STRING);
}

Bitstream& Bitstream::operator>(QScriptString& string) {
    QString rawString;
    *this >> rawString;
    string = (rawString == INVALID_STRING) ? QScriptString() :
        ScriptCache::getInstance()->getEngine()->toStringHandle(rawString);
    return *this;
}

Bitstream& Bitstream::operator<(const SharedObjectPointer& object) {
    if (!object) {
        return *this << (int)0;
    }
    *this << object->getID();
    *this << object->getOriginID();
    QPointer<SharedObject> reference = _sharedObjectReferences.value(object->getOriginID());
    if (reference) {
        writeRawDelta((const QObject*)object.data(), (const QObject*)reference.data());
    } else {
        *this << (QObject*)object.data();
    }
    return *this;
}

Bitstream& Bitstream::operator>(SharedObjectPointer& object) {
    int id;
    *this >> id;
    if (id == 0) {
        object = SharedObjectPointer();
        return *this;
    }
    int originID;
    *this >> originID;
    QPointer<SharedObject> reference = _sharedObjectReferences.value(originID);
    QPointer<SharedObject>& pointer = _weakSharedObjectHash[id];
    if (pointer) {
        ObjectReader objectReader;
        _metaObjectStreamer >> objectReader;
        if (reference) {
            objectReader.readDelta(*this, reference.data(), pointer.data());
        } else {
            objectReader.read(*this, pointer.data());
        }
    } else {
        QObject* rawObject; 
        if (reference) {
            readRawDelta(rawObject, (const QObject*)reference.data());
        } else {
            *this >> rawObject;
        }
        pointer = static_cast<SharedObject*>(rawObject);
        if (pointer) {
            if (reference) {
                pointer->setOriginID(reference->getOriginID());
            }
            pointer->setRemoteID(id);
            pointer->setRemoteOriginID(originID);
        } else {
            qDebug() << "Null object" << pointer << reference << id;
        }
    }
    object = static_cast<SharedObject*>(pointer.data());
    return *this;
}

void Bitstream::clearSharedObject(QObject* object) {
    SharedObject* sharedObject = static_cast<SharedObject*>(object);
    _sharedObjectReferences.remove(sharedObject->getOriginID());
    int id = _sharedObjectStreamer.takePersistentID(sharedObject);
    if (id != 0) {
        emit sharedObjectCleared(id);
    }
}

QHash<QByteArray, const QMetaObject*>& Bitstream::getMetaObjects() {
    static QHash<QByteArray, const QMetaObject*> metaObjects;
    return metaObjects;
}

QMultiHash<const QMetaObject*, const QMetaObject*>& Bitstream::getMetaObjectSubClasses() {
    static QMultiHash<const QMetaObject*, const QMetaObject*> metaObjectSubClasses;
    return metaObjectSubClasses;
}

QHash<int, const TypeStreamer*>& Bitstream::getTypeStreamers() {
    static QHash<int, const TypeStreamer*> typeStreamers;
    return typeStreamers;
}

const QHash<ScopeNamePair, const TypeStreamer*>& Bitstream::getEnumStreamers() {
    static QHash<ScopeNamePair, const TypeStreamer*> enumStreamers = createEnumStreamers();
    return enumStreamers;
}

QHash<ScopeNamePair, const TypeStreamer*> Bitstream::createEnumStreamers() {
    QHash<ScopeNamePair, const TypeStreamer*> enumStreamers;
    foreach (const QMetaObject* metaObject, getMetaObjects()) {
        for (int i = 0; i < metaObject->enumeratorCount(); i++) {
            QMetaEnum metaEnum = metaObject->enumerator(i);
            const TypeStreamer*& streamer = enumStreamers[ScopeNamePair(metaEnum.scope(), metaEnum.name())];
            if (!streamer) {
                streamer = new EnumTypeStreamer(metaEnum);
            }
        }
    }
    return enumStreamers;
}

const QHash<QByteArray, const TypeStreamer*>& Bitstream::getEnumStreamersByName() {
    static QHash<QByteArray, const TypeStreamer*> enumStreamersByName = createEnumStreamersByName();
    return enumStreamersByName;
}

QHash<QByteArray, const TypeStreamer*> Bitstream::createEnumStreamersByName() {
    QHash<QByteArray, const TypeStreamer*> enumStreamersByName;
    foreach (const TypeStreamer* streamer, getEnumStreamers()) {
        enumStreamersByName.insert(streamer->getName(), streamer);
    }
    return enumStreamersByName;
}

const QHash<const QMetaObject*, PropertyReaderVector>& Bitstream::getPropertyReaders() {
    static QHash<const QMetaObject*, PropertyReaderVector> propertyReaders = createPropertyReaders();
    return propertyReaders;
}

QHash<const QMetaObject*, PropertyReaderVector> Bitstream::createPropertyReaders() {
    QHash<const QMetaObject*, PropertyReaderVector> propertyReaders;
    foreach (const QMetaObject* metaObject, getMetaObjects()) {
        PropertyReaderVector& readers = propertyReaders[metaObject];
        for (int i = 0; i < metaObject->propertyCount(); i++) {
            QMetaProperty property = metaObject->property(i);
            if (!property.isStored()) {
                continue;
            }
            const TypeStreamer* streamer;
            if (property.isEnumType()) {
                QMetaEnum metaEnum = property.enumerator();
                streamer = getEnumStreamers().value(ScopeNamePair(
                    QByteArray::fromRawData(metaEnum.scope(), strlen(metaEnum.scope())),
                    QByteArray::fromRawData(metaEnum.name(), strlen(metaEnum.name()))));
            } else {
                streamer = getTypeStreamers().value(property.userType());    
            }
            if (streamer) {
                readers.append(PropertyReader(TypeReader(QByteArray(), streamer), property));
            }
        }
    }
    return propertyReaders;
}

const QHash<const QMetaObject*, PropertyWriterVector>& Bitstream::getPropertyWriters() {
    static QHash<const QMetaObject*, PropertyWriterVector> propertyWriters = createPropertyWriters();
    return propertyWriters;
}

QHash<const QMetaObject*, PropertyWriterVector> Bitstream::createPropertyWriters() {
    QHash<const QMetaObject*, PropertyWriterVector> propertyWriters;
    foreach (const QMetaObject* metaObject, getMetaObjects()) {
        PropertyWriterVector& writers = propertyWriters[metaObject];
        for (int i = 0; i < metaObject->propertyCount(); i++) {
            QMetaProperty property = metaObject->property(i);
            if (!property.isStored()) {
                continue;
            }
            const TypeStreamer* streamer;
            if (property.isEnumType()) {
                QMetaEnum metaEnum = property.enumerator();
                streamer = getEnumStreamers().value(ScopeNamePair(
                    QByteArray::fromRawData(metaEnum.scope(), strlen(metaEnum.scope())),
                    QByteArray::fromRawData(metaEnum.name(), strlen(metaEnum.name()))));
            } else {
                streamer = getTypeStreamers().value(property.userType());    
            }
            if (streamer) {
                writers.append(PropertyWriter(property, streamer));
            }
        }
    }
    return propertyWriters;
}

TypeReader::TypeReader(const QByteArray& typeName, const TypeStreamer* streamer) :
    _typeName(typeName),
    _streamer(streamer),
    _exactMatch(true) {
}

TypeReader::TypeReader(const QByteArray& typeName, const TypeStreamer* streamer, int bits, const QHash<int, int>& mappings) :
    _typeName(typeName),
    _streamer(streamer),
    _exactMatch(false),
    _type(ENUM_TYPE),
    _bits(bits),
    _mappings(mappings) {
}

TypeReader::TypeReader(const QByteArray& typeName, const TypeStreamer* streamer, const QVector<FieldReader>& fields) :
    _typeName(typeName),
    _streamer(streamer),
    _exactMatch(false),
    _type(STREAMABLE_TYPE),
    _fields(fields) {
}

TypeReader::TypeReader(const QByteArray& typeName, const TypeStreamer* streamer,
        Type type, const TypeReaderPointer& valueReader) :
    _typeName(typeName),
    _streamer(streamer),
    _exactMatch(false),
    _type(type),
    _valueReader(valueReader) {
}

TypeReader::TypeReader(const QByteArray& typeName, const TypeStreamer* streamer,
        const TypeReaderPointer& keyReader, const TypeReaderPointer& valueReader) :
    _typeName(typeName),
    _streamer(streamer),
    _exactMatch(false),
    _type(MAP_TYPE),
    _keyReader(keyReader),
    _valueReader(valueReader) {
}

QVariant TypeReader::read(Bitstream& in) const {
    if (_exactMatch) {
        return _streamer->read(in);
    }
    QVariant object = _streamer ? QVariant(_streamer->getType(), 0) : QVariant();
    switch (_type) {
        case ENUM_TYPE: {
            int value = 0;
            in.read(&value, _bits);
            if (_streamer) {
                _streamer->setEnumValue(object, value, _mappings);
            }
            break;
        }
        case STREAMABLE_TYPE: {
            foreach (const FieldReader& field, _fields) {
                field.read(in, _streamer, object);
            }
            break;
        }
        case LIST_TYPE:
        case SET_TYPE: {
            int size;
            in >> size;
            for (int i = 0; i < size; i++) {
                QVariant value = _valueReader->read(in);
                if (_streamer) {
                    _streamer->insert(object, value);
                }
            }
            break;
        }
        case MAP_TYPE: {
            int size;
            in >> size;
            for (int i = 0; i < size; i++) {
                QVariant key = _keyReader->read(in);
                QVariant value = _valueReader->read(in);
                if (_streamer) {
                    _streamer->insert(object, key, value);
                }
            }
            break;
        }
        default:
            break;
    }
    return object;
}

void TypeReader::readDelta(Bitstream& in, QVariant& object, const QVariant& reference) const {
    if (_exactMatch) {
        _streamer->readDelta(in, object, reference);
        return;
    }
    bool changed;
    in >> changed;
    if (changed) {
        readRawDelta(in, object, reference);
    } else {
        object = reference;
    }
}

void TypeReader::readRawDelta(Bitstream& in, QVariant& object, const QVariant& reference) const {
    if (_exactMatch) {
        _streamer->readRawDelta(in, object, reference);
        return;
    }
    switch (_type) {
        case ENUM_TYPE: {
            int value = 0;
            in.read(&value, _bits);
            if (_streamer) {
                _streamer->setEnumValue(object, value, _mappings);
            }
            break;
        }
        case STREAMABLE_TYPE: {
            foreach (const FieldReader& field, _fields) {
                field.readDelta(in, _streamer, object, reference);
            }
            break;
        }
        case LIST_TYPE: {
            object = reference;
            int size, referenceSize;
            in >> size >> referenceSize;
            if (_streamer) {
                if (size < referenceSize) {
                    _streamer->prune(object, size);
                }
                for (int i = 0; i < size; i++) {
                    if (i < referenceSize) {
                        QVariant value;
                        _valueReader->readDelta(in, value, _streamer->getValue(reference, i));
                        _streamer->setValue(object, i, value);
                    } else {
                        _streamer->insert(object, _valueReader->read(in));
                    }
                }
            } else {
                for (int i = 0; i < size; i++) {
                    if (i < referenceSize) {
                        QVariant value;
                        _valueReader->readDelta(in, value, QVariant());
                    } else {
                        _valueReader->read(in);
                    }
                }
            }
            break;
        }
        case SET_TYPE: {
            object = reference;
            int addedOrRemoved;
            in >> addedOrRemoved;
            for (int i = 0; i < addedOrRemoved; i++) {
                QVariant value = _valueReader->read(in);
                if (_streamer && !_streamer->remove(object, value)) {
                    _streamer->insert(object, value);
                }
            }
            break;
        }
        case MAP_TYPE: {
            object = reference;
            int added;
            in >> added;
            for (int i = 0; i < added; i++) {
                QVariant key = _keyReader->read(in);
                QVariant value = _valueReader->read(in);
                if (_streamer) {
                    _streamer->insert(object, key, value);
                }
            }
            int modified;
            in >> modified;
            for (int i = 0; i < modified; i++) {
                QVariant key = _keyReader->read(in);
                QVariant value;
                if (_streamer) {
                    _valueReader->readDelta(in, value, _streamer->getValue(reference, key));
                    _streamer->insert(object, key, value);
                } else {
                    _valueReader->readDelta(in, value, QVariant());
                }
            }
            int removed;
            in >> removed;
            for (int i = 0; i < removed; i++) {
                QVariant key = _keyReader->read(in);
                if (_streamer) {
                    _streamer->remove(object, key);
                }
            }
            break;
        }
        default:
            break;
    }
}

bool TypeReader::matchesExactly(const TypeStreamer* streamer) const {
    return _exactMatch && _streamer == streamer;
}

uint qHash(const TypeReader& typeReader, uint seed) {
    return qHash(typeReader.getTypeName(), seed);
}

QDebug& operator<<(QDebug& debug, const TypeReader& typeReader) {
    return debug << typeReader.getTypeName();
}

FieldReader::FieldReader(const TypeReader& reader, int index) :
    _reader(reader),
    _index(index) {
}

void FieldReader::read(Bitstream& in, const TypeStreamer* streamer, QVariant& object) const {
    QVariant value = _reader.read(in);
    if (_index != -1 && streamer) {
        streamer->setField(object, _index, value);
    }    
}

void FieldReader::readDelta(Bitstream& in, const TypeStreamer* streamer, QVariant& object, const QVariant& reference) const {
    QVariant value;
    if (_index != -1 && streamer) {
        _reader.readDelta(in, value, streamer->getField(reference, _index));
        streamer->setField(object, _index, value);
    } else {
        _reader.readDelta(in, value, QVariant());
    }
}

ObjectReader::ObjectReader(const QByteArray& className, const QMetaObject* metaObject,
        const PropertyReaderVector& properties) :
    _className(className),
    _metaObject(metaObject),
    _properties(properties) {
}

QObject* ObjectReader::read(Bitstream& in, QObject* object) const {
    if (!object && _metaObject) {
        object = _metaObject->newInstance();
    }
    foreach (const PropertyReader& property, _properties) {
        property.read(in, object);
    }
    return object;
}

QObject* ObjectReader::readDelta(Bitstream& in, const QObject* reference, QObject* object) const {
    if (!object && _metaObject) {
        object = _metaObject->newInstance();
    }
    foreach (const PropertyReader& property, _properties) {
        property.readDelta(in, object, reference);
    }
    return object;
}

uint qHash(const ObjectReader& objectReader, uint seed) {
    return qHash(objectReader.getClassName(), seed);
}

QDebug& operator<<(QDebug& debug, const ObjectReader& objectReader) {
    return debug << objectReader.getClassName();
}

PropertyReader::PropertyReader(const TypeReader& reader, const QMetaProperty& property) :
    _reader(reader),
    _property(property) {
}

void PropertyReader::read(Bitstream& in, QObject* object) const {
    QVariant value = _reader.read(in);
    if (_property.isValid() && object) {
        _property.write(object, value);
    }
}

void PropertyReader::readDelta(Bitstream& in, QObject* object, const QObject* reference) const {
    QVariant value;
    _reader.readDelta(in, value, (_property.isValid() && reference) ? _property.read(reference) : QVariant());
    if (_property.isValid() && object) {
        _property.write(object, value);
    }
}

PropertyWriter::PropertyWriter(const QMetaProperty& property, const TypeStreamer* streamer) :
    _property(property),
    _streamer(streamer) {
}

void PropertyWriter::write(Bitstream& out, const QObject* object) const {
    _streamer->write(out, _property.read(object));
}

void PropertyWriter::writeDelta(Bitstream& out, const QObject* object, const QObject* reference) const {
    _streamer->writeDelta(out, _property.read(object), reference && object->metaObject() == reference->metaObject() ?
        _property.read(reference) : QVariant());
}

MetaField::MetaField(const QByteArray& name, const TypeStreamer* streamer) :
    _name(name),
    _streamer(streamer) {
}

TypeStreamer::~TypeStreamer() {
}

const char* TypeStreamer::getName() const {
    return QMetaType::typeName(_type);
}

const TypeStreamer* TypeStreamer::getStreamerToWrite(const QVariant& value) const {
    return this;
}

bool TypeStreamer::equal(const QVariant& first, const QVariant& second) const {
    return first == second;
}

void TypeStreamer::write(Bitstream& out, const QVariant& value) const {
    // nothing by default
}

QVariant TypeStreamer::read(Bitstream& in) const {
    return QVariant();
}

void TypeStreamer::writeDelta(Bitstream& out, const QVariant& value, const QVariant& reference) const {
    if (value == reference) {
        out << false;
    } else {
        out << true;
        writeRawDelta(out, value, reference);
    }
}

void TypeStreamer::readDelta(Bitstream& in, QVariant& value, const QVariant& reference) const {
    bool changed;
    in >> changed;
    if (changed) {
        readRawDelta(in, value, reference);
    } else {
        value = reference;
    }
}

void TypeStreamer::writeRawDelta(Bitstream& out, const QVariant& value, const QVariant& reference) const {
    // nothing by default
}

void TypeStreamer::readRawDelta(Bitstream& in, QVariant& value, const QVariant& reference) const {
    value = reference;
}

void TypeStreamer::setEnumValue(QVariant& object, int value, const QHash<int, int>& mappings) const {
    // nothing by default
}

const QVector<MetaField>& TypeStreamer::getMetaFields() const {
    static QVector<MetaField> emptyMetaFields;
    return emptyMetaFields;
}

int TypeStreamer::getFieldIndex(const QByteArray& name) const {
    return -1;
}

void TypeStreamer::setField(QVariant& object, int index, const QVariant& value) const {
    // nothing by default
}

QVariant TypeStreamer::getField(const QVariant& object, int index) const {
    return QVariant();
}

TypeReader::Type TypeStreamer::getReaderType() const {
    return TypeReader::SIMPLE_TYPE;
}

int TypeStreamer::getBits() const {
    return 0;
}

QMetaEnum TypeStreamer::getMetaEnum() const {
    return QMetaEnum();
}

const TypeStreamer* TypeStreamer::getKeyStreamer() const {
    return NULL;
}

const TypeStreamer* TypeStreamer::getValueStreamer() const {
    return NULL;
}

void TypeStreamer::insert(QVariant& object, const QVariant& element) const {
    // nothing by default
}

void TypeStreamer::insert(QVariant& object, const QVariant& key, const QVariant& value) const {
    // nothing by default
}

bool TypeStreamer::remove(QVariant& object, const QVariant& key) const {
    return false;
}

QVariant TypeStreamer::getValue(const QVariant& object, const QVariant& key) const {
    return QVariant();
}

void TypeStreamer::prune(QVariant& object, int size) const {
    // nothing by default
}

QVariant TypeStreamer::getValue(const QVariant& object, int index) const {
    return QVariant();
}

void TypeStreamer::setValue(QVariant& object, int index, const QVariant& value) const {
    // nothing by default
}

QDebug& operator<<(QDebug& debug, const TypeStreamer* typeStreamer) {
    return debug << (typeStreamer ? QMetaType::typeName(typeStreamer->getType()) : "null");
}

QDebug& operator<<(QDebug& debug, const QMetaObject* metaObject) {
    return debug << (metaObject ? metaObject->className() : "null");
}

EnumTypeStreamer::EnumTypeStreamer(const QMetaObject* metaObject, const char* name) :
    _metaObject(metaObject),
    _enumName(name),
    _name(QByteArray(metaObject->className()) + "::" + name),
    _bits(-1) {
    
    _type = QMetaType::Int;
    _self = TypeStreamerPointer(this);
}

EnumTypeStreamer::EnumTypeStreamer(const QMetaEnum& metaEnum) :
    _name(QByteArray(metaEnum.scope()) + "::" + metaEnum.name()),
    _metaEnum(metaEnum),
    _bits(-1) {
    
    _type = QMetaType::Int;
    _self = TypeStreamerPointer(this);
}

const char* EnumTypeStreamer::getName() const {
    return _name.constData();
}

TypeReader::Type EnumTypeStreamer::getReaderType() const {
    return TypeReader::ENUM_TYPE;
}

int EnumTypeStreamer::getBits() const {
    if (_bits == -1) {
        int highestValue = 0;
        QMetaEnum metaEnum = getMetaEnum();
        for (int j = 0; j < metaEnum.keyCount(); j++) {
            highestValue = qMax(highestValue, metaEnum.value(j));
        }
        const_cast<EnumTypeStreamer*>(this)->_bits = getBitsForHighestValue(highestValue);
    }
    return _bits;
}

QMetaEnum EnumTypeStreamer::getMetaEnum() const {
    if (!_metaEnum.isValid()) {
        const_cast<EnumTypeStreamer*>(this)->_metaEnum = _metaObject->enumerator(_metaObject->indexOfEnumerator(_enumName));
    }
    return _metaEnum;
}

bool EnumTypeStreamer::equal(const QVariant& first, const QVariant& second) const {
    return first.toInt() == second.toInt();
}

void EnumTypeStreamer::write(Bitstream& out, const QVariant& value) const {
    int intValue = value.toInt();
    out.write(&intValue, getBits());
}

QVariant EnumTypeStreamer::read(Bitstream& in) const {
    int intValue = 0;
    in.read(&intValue, getBits());
    return intValue;
}

void EnumTypeStreamer::writeDelta(Bitstream& out, const QVariant& value, const QVariant& reference) const {
    int intValue = value.toInt(), intReference = reference.toInt();
    if (intValue == intReference) {
        out << false;
    } else {
        out << true;
        out.write(&intValue, getBits());
    }
}

void EnumTypeStreamer::readDelta(Bitstream& in, QVariant& value, const QVariant& reference) const {
    bool changed;
    in >> changed;
    if (changed) {
        int intValue = 0;
        in.read(&intValue, getBits());
        value = intValue;
    } else {
        value = reference;
    }
}

void EnumTypeStreamer::writeRawDelta(Bitstream& out, const QVariant& value, const QVariant& reference) const {
    int intValue = value.toInt();
    out.write(&intValue, getBits());
}

void EnumTypeStreamer::readRawDelta(Bitstream& in, QVariant& value, const QVariant& reference) const {
    int intValue = 0;
    in.read(&intValue, getBits());
    value = intValue;
}

void EnumTypeStreamer::setEnumValue(QVariant& object, int value, const QHash<int, int>& mappings) const {
    if (getMetaEnum().isFlag()) {
        int combined = 0;
        for (QHash<int, int>::const_iterator it = mappings.constBegin(); it != mappings.constEnd(); it++) {
            if (value & it.key()) {
                combined |= it.value();
            }
        }
        object = combined;
        
    } else {
        object = mappings.value(value);
    }
}

MappedEnumTypeStreamer::MappedEnumTypeStreamer(const TypeStreamer* baseStreamer, int bits, const QHash<int, int>& mappings) :
    _baseStreamer(baseStreamer),
    _bits(bits),
    _mappings(mappings) {
}

QVariant MappedEnumTypeStreamer::read(Bitstream& in) const {
    QVariant object = _baseStreamer ? QVariant(_baseStreamer->getType(), 0) : QVariant();
    int value = 0;
    in.read(&value, _bits);
    if (_baseStreamer) {
        _baseStreamer->setEnumValue(object, value, _mappings);
    }
    return object;
}

void MappedEnumTypeStreamer::readRawDelta(Bitstream& in, QVariant& object, const QVariant& reference) const {
    int value = 0;
    in.read(&value, _bits);
    if (_baseStreamer) {
        _baseStreamer->setEnumValue(object, value, _mappings);
    }
}

GenericValue::GenericValue(const TypeStreamerPointer& streamer, const QVariant& value) :
    _streamer(streamer),
    _value(value) {
}

bool GenericValue::operator==(const GenericValue& other) const {
    return _streamer == other._streamer && _value == other._value;
}

const TypeStreamer* GenericTypeStreamer::getStreamerToWrite(const QVariant& value) const {
    return value.value<GenericValue>().getStreamer().data();
}

MappedListTypeStreamer::MappedListTypeStreamer(const TypeStreamer* baseStreamer, const TypeStreamerPointer& valueStreamer) :
    _baseStreamer(baseStreamer),
    _valueStreamer(valueStreamer) {
}

QVariant MappedListTypeStreamer::read(Bitstream& in) const {
    QVariant object = _baseStreamer ? QVariant(_baseStreamer->getType(), 0) : QVariant();
    int size;
    in >> size;
    for (int i = 0; i < size; i++) {
        QVariant value = _valueStreamer->read(in);
        if (_baseStreamer) {
            _baseStreamer->insert(object, value);
        }
    }
    return object;
}

void MappedListTypeStreamer::readRawDelta(Bitstream& in, QVariant& object, const QVariant& reference) const {
    object = reference;
    int size, referenceSize;
    in >> size >> referenceSize;
    if (_baseStreamer) {
        if (size < referenceSize) {
            _baseStreamer->prune(object, size);
        }
        for (int i = 0; i < size; i++) {
            if (i < referenceSize) {
                QVariant value;
                _valueStreamer->readDelta(in, value, _baseStreamer->getValue(reference, i));
                _baseStreamer->setValue(object, i, value);
            } else {
                _baseStreamer->insert(object, _valueStreamer->read(in));
            }
        }
    } else {
        for (int i = 0; i < size; i++) {
            if (i < referenceSize) {
                QVariant value;
                _valueStreamer->readDelta(in, value, QVariant());
            } else {
                _valueStreamer->read(in);
            }
        }
    }
}

MappedSetTypeStreamer::MappedSetTypeStreamer(const TypeStreamer* baseStreamer, const TypeStreamerPointer& valueStreamer) :
    MappedListTypeStreamer(baseStreamer, valueStreamer) {
}

void MappedSetTypeStreamer::readRawDelta(Bitstream& in, QVariant& object, const QVariant& reference) const {
    object = reference;
    int addedOrRemoved;
    in >> addedOrRemoved;
    for (int i = 0; i < addedOrRemoved; i++) {
        QVariant value = _valueStreamer->read(in);
        if (_baseStreamer && !_baseStreamer->remove(object, value)) {
            _baseStreamer->insert(object, value);
        }
    }
}

MappedMapTypeStreamer::MappedMapTypeStreamer(const TypeStreamer* baseStreamer, const TypeStreamerPointer& keyStreamer,
        const TypeStreamerPointer& valueStreamer) :
    _baseStreamer(baseStreamer),
    _keyStreamer(keyStreamer),
    _valueStreamer(valueStreamer) {
}

QVariant MappedMapTypeStreamer::read(Bitstream& in) const {
    QVariant object = _baseStreamer ? QVariant(_baseStreamer->getType(), 0) : QVariant();
    int size;
    in >> size;
    for (int i = 0; i < size; i++) {
        QVariant key = _keyStreamer->read(in);
        QVariant value = _valueStreamer->read(in);
        if (_baseStreamer) {
            _baseStreamer->insert(object, key, value);
        }
    }
    return object;
}

void MappedMapTypeStreamer::readRawDelta(Bitstream& in, QVariant& object, const QVariant& reference) const {
    object = reference;
    int added;
    in >> added;
    for (int i = 0; i < added; i++) {
        QVariant key = _keyStreamer->read(in);
        QVariant value = _valueStreamer->read(in);
        if (_baseStreamer) {
            _baseStreamer->insert(object, key, value);
        }
    }
    int modified;
    in >> modified;
    for (int i = 0; i < modified; i++) {
        QVariant key = _keyStreamer->read(in);
        QVariant value;
        if (_baseStreamer) {
            _valueStreamer->readDelta(in, value, _baseStreamer->getValue(reference, key));
            _baseStreamer->insert(object, key, value);
        } else {
            _valueStreamer->readDelta(in, value, QVariant());
        }
    }
    int removed;
    in >> removed;
    for (int i = 0; i < removed; i++) {
        QVariant key = _keyStreamer->read(in);
        if (_baseStreamer) {
            _baseStreamer->remove(object, key);
        }
    }
}

