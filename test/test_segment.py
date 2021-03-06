#!/usr/bin/env python
import os, sys
os.environ['CUDA_VISIBLE_DEVICES'] = '0'

import numpy as np
import tensorflow as tf
import cv2
import pickle

from test_utils import *
bilateral_filters = load_func_from_lib()

path2file = os.path.dirname(os.path.realpath(__file__))
#---------------------------------------------------------------------
# setup a test

BATCHMODE = True

def myimshow(wname, im):
    if (im.shape[0]*im.shape[1]) < (50*50):
        cv2.imshow(wname, cv2.resize(im, (0,0), fx=2.0, fy=2.0, interpolation=cv2.INTER_NEAREST))
    else:
        cv2.imshow(wname, im)

imfile = os.path.join(path2file, 'orange_target.png')
train_x, train_y = load_4channel_truth_img(imfile)

xmean = np.mean(train_x, axis=(1,2), keepdims=True)
train_x -= xmean

if BATCHMODE:
    train_x = np.tile(train_x,(2,1,1,1))
    train_y = np.tile(train_y,(2,1,1,1))

myvars = {}
myvars['xxx'] = train_x
myvars['wrt'] = train_x
myvars['yyy'] = train_y
myvars['wsp'] = 1.0
myvars['wbi'] = 1.0

myimshow("train_x",(train_x+xmean)[0,...])
#myimshow("train_y_0",train_y[0,:,:,0])
myimshow("train_y_1",train_y[0,:,:,1])

describe("train_x",train_x)
describe("train_y",train_y)

tf_x_placehold = tf.placeholder(tf.float32, train_x.shape, name="tf_x_placehold")
tf_y_placehold = tf.placeholder(tf.float32, train_y.shape, name="tf_x_placehold")

#----------------------------------------
useCRF = True
LEARNRATE = 0.01
NUMITERS = 10000

comp_class = conv1x1(tf_x_placehold,  3, 2, None,      "comp_class")
#----------------------------------------

crfprescale  = None
tfwspatial   = None
tfwbilateral = None
if not useCRF:
    print("NOT using crf")
    finalpred_logits = comp_class
else:
    stdv_space = 3.
    stdv_color = 1.2
    print("using CRF")
    #crfprescale  = tf.get_variable('crfprescale', initializer=tf.constant(1.0))
    tfwspatial   = tf.get_variable('tfwspatial',  initializer=tf.constant(1.0))
    tfwbilateral = tf.get_variable('tfwbilateral',initializer=tf.constant(1.0))

    reshcc = NHWC_to_NCHW(comp_class)# * crfprescale)
    outbilat = bilateral_filters(input=reshcc, #input
                            featswrt=NHWC_to_NCHW(tf_x_placehold), #featswrt
                            stdv_space=stdv_space,
                            stdv_color=stdv_color)
    finalpred_logits = NCHW_to_NHWC(outbilat * tfwbilateral)

finalpred_softmax = tf.nn.softmax(finalpred_logits)
total_loss = tf.reduce_mean(tf.nn.softmax_cross_entropy_with_logits(logits=finalpred_logits, labels=tf_y_placehold))

train_step = tf.train.AdamOptimizer(LEARNRATE).minimize(total_loss)

if useCRF:
    print("constructed the CRF filter!!!")
describe("tf_x_placehold",tf_x_placehold)
describe("tf_y_placehold",tf_y_placehold)
describe("finalpred_logits",finalpred_logits)
describe("tfwspatial",tfwspatial)
describe("tfwbilateral",tfwbilateral)
print("\n")

#---------------------------------------------------------------------
# run the test

config = tf.ConfigProto()
config.gpu_options.allow_growth=True
sess = tf.InteractiveSession(config=config)
sess.run(tf.initialize_all_variables())

feeddict_xy = {tf_x_placehold: train_x, tf_y_placehold: train_y}
feeddict_x  = {tf_x_placehold: train_x}

try:
    for ii in range(NUMITERS):
        thisloss = sess.run([total_loss, train_step], feeddict_xy)[0]
        print("iter "+str(ii)+" loss: "+str(thisloss))
        if ii % 2 == 0:
            these_preds = finalpred_softmax.eval(feeddict_x)
            myimshow("preds",these_preds[0,:,:,1])
            cv2.waitKey(100)
except KeyboardInterrupt:
    trainablevars = tf.trainable_variables()
    varsdict = {}
    varsdict['imagefile'] = imfile
    varsdict['stdv_space'] = stdv_space
    varsdict['stdv_color'] = stdv_color
    for tvar in trainablevars:
        varsdict[tvar.name] = tvar.eval()
        print(tvar.name+" has value:")
        print(str(varsdict[tvar.name]))
        print(" ")
    pickle.dump(varsdict, open("trained_segment_weights.pkl", "wb"))
