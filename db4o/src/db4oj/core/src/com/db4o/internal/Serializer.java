/* Copyright (C) 2004 - 2008  db4objects Inc.  http://www.db4o.com

This file is part of the db4o open source object database.

db4o is free software; you can redistribute it and/or modify it under
the terms of version 2 of the GNU General Public License as published
by the Free Software Foundation and as clarified by db4objects' GPL 
interpretation policy, available at
http://www.db4o.com/about/company/legalpolicies/gplinterpretation/
Alternatively you can write to db4objects, Inc., 1900 S Norfolk Street,
Suite 350, San Mateo, CA 94403, USA.

db4o is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
59 Temple Place - Suite 330, Boston, MA  02111-1307, USA. */
package com.db4o.internal;

import com.db4o.ext.*;
import com.db4o.internal.activation.*;


/**
 * @exclude
 */
public class Serializer {
	
    public static StatefulBuffer marshall(Transaction ta, Object obj) {
        SerializedGraph serialized = marshall(ta.container(), obj);
        StatefulBuffer buffer = new StatefulBuffer(ta, serialized.length());
        buffer.append(serialized._bytes);
        buffer.useSlot(serialized._id, 0, serialized.length());
        return buffer;
    }

    public static SerializedGraph marshall(ObjectContainerBase serviceProvider, Object obj) {
        MemoryFile memoryFile = new MemoryFile();
        memoryFile.setInitialSize(223);
        memoryFile.setIncrementSizeBy(300);
    	TransportObjectContainer carrier = new TransportObjectContainer(serviceProvider, memoryFile);
    	carrier.produceClassMetadata(carrier.reflector().forObject(obj));
		carrier.store(obj);
		int id = (int)carrier.getID(obj);
		carrier.close();
		return new SerializedGraph(id, memoryFile.getBytes());
    }
    
    public static Object unmarshall(ObjectContainerBase serviceProvider, StatefulBuffer yapBytes) {
        return unmarshall(serviceProvider, yapBytes._buffer, yapBytes.getID());
    }
    
    public static Object unmarshall(ObjectContainerBase serviceProvider, SerializedGraph serialized) {
    	return unmarshall(serviceProvider, serialized._bytes, serialized._id);
    }

    public static Object unmarshall(ObjectContainerBase serviceProvider, byte[] bytes, int id) {
		if(id <= 0){
			return null;
		}
        MemoryFile memoryFile = new MemoryFile(bytes);
		TransportObjectContainer carrier = new TransportObjectContainer(serviceProvider, memoryFile);
		Object obj = carrier.getByID(id);
		carrier.activate(carrier.transaction(), obj, new FullActivationDepth());
		carrier.close();
		return obj;
    }

}